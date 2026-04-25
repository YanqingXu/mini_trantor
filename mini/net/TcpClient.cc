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
      connect_(true),
      nextConnId_(1),
      hostname_(std::move(hostname)),
      port_(port),
      resolver_(resolver ? std::move(resolver) : DnsResolver::getShared()),
      resolveGuard_(std::make_shared<bool>(true)) {
}

TcpClient::TcpClient(EventLoop* loop, const InetAddress& serverAddr, std::string name, TcpClientOptions options)
    : loop_(loop),
      name_(std::move(name)),
      retry_(options.retry),
      connect_(true),
      nextConnId_(1),
      connectorOptions_(options.connector) {
    initConnector(serverAddr, options.connector);
}

TcpClient::~TcpClient() {
    loop_->assertInLoopThread();

    // Invalidate pending DNS resolve callback.
    if (resolveGuard_) {
        *resolveGuard_ = false;
    }

    TcpConnectionPtr conn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conn = connection_;
    }

    if (conn) {
        // Detach connection from this client: clear close callback,
        // then destroy the connection on its owner loop.
        EventLoop* ioLoop = conn->getLoop();
        ioLoop->runInLoop([conn] {
            conn->setCloseCallback({});
            conn->connectDestroyed();
        });
    }

    if (connector_) {
        connector_->stop();
    }
}

void TcpClient::connect() {
    connect_ = true;
    if (!hostname_.empty() && !connector_) {
        resolveAndConnect();
    } else {
        connector_->start();
    }
}

void TcpClient::disconnect() {
    connect_ = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connection_) {
            connection_->shutdown();
        }
    }
}

void TcpClient::stop() {
    connect_ = false;
    if (connector_) {
        connector_->stop();
    }
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
    if (connector_) {
        connector_->setConnectorEventCallback(connectorEventCallback_);
    }
}

void TcpClient::setConnectionEventCallback(ConnectionEventCallback cb) {
    connectionEventCallback_ = std::move(cb);
}

void TcpClient::setTlsEventCallback(TlsEventCallback cb) {
    tlsEventCallback_ = std::move(cb);
}

// ── Internal ──

void TcpClient::newConnection(int sockfd) {
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

    // Close callback with hook.
    auto tlsEventCb = tlsEventCallback_;
    conn->setCloseCallback([this, connEventCb](const TcpConnectionPtr& c) {
        if (connEventCb) {
            connEventCb(c, ConnectionEvent::Disconnected);
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

    if (retry_ && connect_) {
        if (connector_) {
            connector_->restart();
        } else {
            // Hostname-based: re-resolve and connect.
            resolveAndConnect();
        }
    }
}

void TcpClient::initConnector(const InetAddress& serverAddr) {
    connector_ = std::make_shared<Connector>(loop_, serverAddr);
    connector_->setNewConnectionCallback([this](int sockfd) { newConnection(sockfd); });
    if (connectorEventCallback_) {
        connector_->setConnectorEventCallback(connectorEventCallback_);
    }
}

void TcpClient::initConnector(const InetAddress& serverAddr, const ConnectorOptions& options) {
    connector_ = std::make_shared<Connector>(loop_, serverAddr, options);
    connector_->setNewConnectionCallback([this](int sockfd) { newConnection(sockfd); });
    if (connectorEventCallback_) {
        connector_->setConnectorEventCallback(connectorEventCallback_);
    }
}

void TcpClient::resolveAndConnect() {
    auto guard = resolveGuard_;
    auto options = connectorOptions_;
    resolver_->resolve(hostname_, port_, loop_,
        [this, guard, options](DnsResolver::ResolveResult addrs) mutable {
            // Delivered on owner loop thread.
            if (!*guard) return;  // TcpClient was destroyed
            if (!connect_) return;  // stop() was called
            if (!addrs) {
                LOG_ERROR << "TcpClient: DNS resolution failed for '" << hostname_ << "'";
                return;
            }
            if (options) {
                initConnector((*addrs)[0], *options);
            } else {
                initConnector((*addrs)[0]);
            }
            connector_->start();
        });
}

}  // namespace mini::net
