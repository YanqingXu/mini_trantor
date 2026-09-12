#include "mini/net/TcpClient.h"

#include "mini/net/Connector.h"
#include "mini/net/DnsResolver.h"
#include "mini/net/EventLoop.h"
#include "mini/net/SocketsOps.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TlsContext.h"

#include "mini/base/Logger.h"

#include <cassert>
#include <utility>

namespace mini::net {

TcpClient::TcpClient(EventLoop* loop, const InetAddress& serverAddr, std::string name)
    : TcpClient(loop, serverAddr, std::move(name), TcpClientOptions{}) {
}

TcpClient::TcpClient(EventLoop* loop, std::string hostname, uint16_t port,
                     std::string name, std::shared_ptr<DnsResolver> resolver)
    : loop_(loop),
      name_(std::move(name)),
      retry_(false),
      connect_(false),
      nextConnId_(1),
      hostname_(std::move(hostname)),
      port_(port),
      resolver_(resolver ? std::move(resolver) : DnsResolver::getShared()) {
}

TcpClient::TcpClient(EventLoop* loop, const InetAddress& serverAddr, std::string name, TcpClientOptions options)
    : loop_(loop),
      name_(std::move(name)),
      retry_(options.retry),
      connect_(false),
      nextConnId_(1),
      fixedAddress_(serverAddr),
      connectorOptions_(options.connector) {
    options.validate();
}

TcpClient::~TcpClient() {
    loop_->assertInLoopThread();

    stopInLoop();
    lifetimeToken_.reset();

    TcpConnectionPtr conn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conn = connection_;
    }

    if (conn) {
        // Detach connection from this client: clear close callback,
        // then destroy the connection on its owner loop.
        EventLoop* ioLoop = conn->getLoop();
        conn->setConnectionCallback({});
        ioLoop->runInLoop([conn] {
            conn->setCloseCallback({});
            conn->connectDestroyed();
        });
    }
}

void TcpClient::connect() {
    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->runInLoop([this, lifetime] {
        if (!lifetime.lock()) {
            return;
        }
        connectInLoop();
    });
}

void TcpClient::connectInLoop() {
    loop_->assertInLoopThread();
    connect_ = true;
    if (connectPhase_ == ConnectPhase::Connected) {
        // Disconnected is notified before the bookkeeping close callback. A
        // manual reconnect from that notification waits for old-map removal.
        auto current = connection();
        if (current && current->disconnected()) {
            std::weak_ptr<void> lifetime = lifetimeToken_;
            const auto generation = connectGeneration_;
            loop_->queueInLoop([this, lifetime, generation] {
                if (!lifetime.lock() || generation != connectGeneration_ || !connect_) { return; }
                connectInLoop();
            });
        }
        return;
    }
    if (connectPhase_ != ConnectPhase::Idle) { return; }

    ++connectGeneration_;
    candidates_.clear();
    nextCandidate_ = 0;
    if (auto previous = std::move(connector_)) { previous->stop(); }
    if (!fixedAddress_) {
        resolveAndConnect();
    } else {
        connectPhase_ = ConnectPhase::Connecting;
        initConnector(*fixedAddress_, connectorOptions_);
        connector_->start();
    }
}

void TcpClient::disconnect() {
    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->runInLoop([this, lifetime] {
        if (!lifetime.lock()) {
            return;
        }
        disconnectInLoop();
    });
}

void TcpClient::disconnectInLoop() {
    loop_->assertInLoopThread();
    stopInLoop();
    TcpConnectionPtr connection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection = connection_;
    }
    if (connection) { connection->shutdown(); }
}

void TcpClient::stop() {
    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->runInLoop([this, lifetime] {
        if (!lifetime.lock()) {
            return;
        }
        stopInLoop();
    });
}

void TcpClient::stopInLoop() {
    loop_->assertInLoopThread();
    connect_ = false;
    ++connectGeneration_;
    candidates_.clear();
    nextCandidate_ = 0;
    // stop does not close an established connection. Keeping Connected prevents
    // a subsequent connect from creating a second connection before its close.
    if (connectPhase_ != ConnectPhase::Connected) { connectPhase_ = ConnectPhase::Idle; }
    if (auto previous = std::move(connector_)) { previous->stop(); }
}

void TcpClient::enableRetry() noexcept {
    retry_ = true;
}

void TcpClient::disableRetry() noexcept {
    retry_ = false;
}

bool TcpClient::retry() const noexcept {
    return retry_;
}

void TcpClient::enableSsl(std::shared_ptr<TlsContext> tlsContext, std::string hostname) {
    tlsContext_ = std::move(tlsContext);
    tlsHostname_ = std::move(hostname);
}

const std::string& TcpClient::name() const noexcept {
    return name_;
}

EventLoop* TcpClient::getLoop() const noexcept {
    return loop_;
}

TcpConnectionPtr TcpClient::connection() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connection_;
}

void TcpClient::setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
}

void TcpClient::setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void TcpClient::setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
}

// ── Metrics hooks ──

void TcpClient::setConnectorEventCallback(ConnectorEventCallback cb) {
    connectorEventCallback_ = std::move(cb);
}

void TcpClient::setConnectionEventCallback(ConnectionEventCallback cb) {
    connectionEventCallback_ = std::move(cb);
}

void TcpClient::setTlsEventCallback(TlsEventCallback cb) {
    tlsEventCallback_ = std::move(cb);
}

// ── Internal ──

void TcpClient::newConnection(SocketFd sockfd) {
    loop_->assertInLoopThread();

    const InetAddress peerAddr(sockets::getPeerAddr(sockfd));
    const InetAddress localAddr(sockets::getLocalAddr(sockfd));
    const std::string connName = name_ + "#" + std::to_string(nextConnId_++);

    auto conn = std::make_shared<TcpConnection>(loop_, connName, sockfd, localAddr, peerAddr);

    // Wrap user's connection callback with ConnectionEvent hook.
    auto connEventCb = connectionEventCallback_;
    auto userConnCb = connectionCallback_;
    conn->setConnectionCallback([userConnCb, connEventCb](const TcpConnectionPtr& c) {
        if (connEventCb) {
            connEventCb(c, c->connected() ? ConnectionEvent::Connected : ConnectionEvent::Disconnected);
        }
        if (userConnCb) {
            userConnCb(c);
        }
    });
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);

    // The connection callback emits Disconnected; close only updates ownership.
    auto tlsEventCb = tlsEventCallback_;
    std::weak_ptr<void> lifetime = lifetimeToken_;
    conn->setCloseCallback([this, lifetime](const TcpConnectionPtr& c) {
        if (!lifetime.lock()) {
            return;
        }
        removeConnection(c);
    });

    // Set TLS event hook if configured.
    if (tlsEventCb) {
        conn->setTlsEventCallback(tlsEventCb);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_ = conn;
    }
    connectPhase_ = ConnectPhase::Connected;
    candidates_.clear();
    nextCandidate_ = 0;

    if (tlsContext_) {
        if (tlsEventCb) {
            tlsEventCb(conn, TlsEvent::HandshakeStarted);
        }
        conn->startTls(tlsContext_, /*isServer=*/false, tlsHostname_);
    }
    conn->connectEstablished();
}

void TcpClient::removeConnection(const TcpConnectionPtr& conn) {
    loop_->assertInLoopThread();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        assert(connection_ == conn);
        connection_.reset();
    }

    loop_->queueInLoop([conn] { conn->connectDestroyed(); });
    connectPhase_ = ConnectPhase::Idle;

    if (retry_ && connect_) { connectInLoop(); }
}

void TcpClient::initConnector(const InetAddress& serverAddr, const ConnectorOptions& options) {
    connector_ = std::make_shared<Connector>(loop_, serverAddr, options);
    std::weak_ptr<void> lifetime = lifetimeToken_;
    std::weak_ptr<Connector> attempt = connector_;
    const auto generation = connectGeneration_;
    connector_->setNewConnectionCallback([this, lifetime, generation, attempt](SocketFd sockfd) {
        if (!lifetime.lock() || generation != connectGeneration_ || !connect_) {
            sockets::close(sockfd);
            return;
        }
        auto current = attempt.lock();
        if (!current || current != connector_) {
            sockets::close(sockfd);
            return;
        }
        newConnection(sockfd);
    });
    connector_->setConnectorEventCallback([this, lifetime, generation, attempt](const InetAddress& address, ConnectorEvent event) {
        if (!lifetime.lock() || generation != connectGeneration_) { return; }
        auto current = attempt.lock();
        if (!current || current != connector_) { return; }
        handleConnectorEvent(address, event, generation, attempt);
    });
}

void TcpClient::resolveAndConnect() {
    connectPhase_ = ConnectPhase::Resolving;
    std::weak_ptr<void> lifetime = lifetimeToken_;
    const auto generation = connectGeneration_;
    resolver_->resolve(hostname_, port_, loop_,
        [this, lifetime, generation](DnsResolver::ResolveResult addrs) {
            // Delivered on owner loop thread.
            if (!lifetime.lock() || generation != connectGeneration_ || !connect_) { return; }
            if (!addrs) {
                connectPhase_ = ConnectPhase::Idle;
                LOG_ERROR << "TcpClient: DNS resolution failed for '" << hostname_ << "'";
                return;
            }
            candidates_ = std::move(*addrs);
            nextCandidate_ = 0;
            startNextCandidate();
        });
}

void TcpClient::startNextCandidate() {
    loop_->assertInLoopThread();
    if (auto previous = std::move(connector_)) { previous->stop(); }
    if (nextCandidate_ == candidates_.size()) {
        candidates_.clear();
        nextCandidate_ = 0;
        connectPhase_ = ConnectPhase::Idle;
        return;
    }
    connectPhase_ = ConnectPhase::Connecting;
    auto options = connectorOptions_;
    options.enableRetry = false; // each candidate gets one attempt, in resolver order
    initConnector(candidates_[nextCandidate_++], options);
    connector_->start();
}

void TcpClient::handleConnectorEvent(const InetAddress& address, ConnectorEvent event,
                                    std::uint64_t generation, std::weak_ptr<Connector> attempt) {
    const bool failed = event == ConnectorEvent::ConnectFailed ||
        event == ConnectorEvent::ConnectTimeout || event == ConnectorEvent::SelfConnectDetected;
    if (failed && !fixedAddress_) {
        // Publish exhaustion before the terminal hook, so an explicit connect()
        // can start a new round. Its generation invalidates the old advance.
        if (nextCandidate_ == candidates_.size()) { connectPhase_ = ConnectPhase::Idle; }
        std::weak_ptr<void> lifetime = lifetimeToken_;
        loop_->queueInLoop([this, lifetime, generation, attempt] {
            if (!lifetime.lock() || generation != connectGeneration_ || !connect_) { return; }
            auto current = attempt.lock();
            if (!current || current != connector_) { return; }
            startNextCandidate();
        });
    } else if (failed) {
        connectPhase_ = ConnectPhase::Idle;
    } else if (event == ConnectorEvent::ConnectAttempt || event == ConnectorEvent::RetryScheduled) {
        connectPhase_ = ConnectPhase::Connecting;
    }

    // The user may replace this hook, stop/reconnect, or destroy the client.
    // Internal advancement is independent and rechecks its generation later.
    auto callback = connectorEventCallback_;
    if (callback) { callback(address, event); }
}

}  // namespace mini::net
