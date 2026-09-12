#include "mini/net/Connector.h"

#include "mini/net/Channel.h"
#include "mini/net/EventLoop.h"
#include "mini/net/Socket.h"
#include "mini/net/SocketsOps.h"
#include "mini/base/Logger.h"

#include <cassert>
#include <cstring>

namespace mini::net {

Connector::Connector(EventLoop* loop, const InetAddress& serverAddr)
    : Connector(loop, serverAddr, ConnectorOptions{}) {
}

Connector::Connector(EventLoop* loop, const InetAddress& serverAddr, ConnectorOptions options)
    : loop_(loop),
      serverAddr_(serverAddr),
      retryEnabled_(options.enableRetry),
      initialRetryDelay_(options.initRetryDelay),
      retryDelayMs_(options.initRetryDelay),
      maxRetryDelayMs_(options.maxRetryDelay),
      connectTimeout_(options.connectTimeout) {
    options.validate();
}

Connector::~Connector() {
    // Inert external references may be released after a worker joins. Any live
    // registration/timer still requires its living owner loop for cleanup.
    if (channel_ || retryTimerId_.valid() || connectTimeoutTimerId_.valid()) {
        loop_->assertInLoopThread();
        cancelTimers();
        if (channel_) { sockets::close(removeAndResetChannel()); }
    }
}

void Connector::setNewConnectionCallback(NewConnectionCallback cb) {
    newConnectionCallback_ = std::move(cb);
}

void Connector::setConnectorEventCallback(ConnectorEventCallback cb) {
    connectorEventCallback_ = std::move(cb);
}

const InetAddress& Connector::serverAddress() const noexcept {
    return serverAddr_;
}

Connector::StateE Connector::state() const noexcept {
    if (phase_ == Phase::Connecting) { return kConnecting; }
    if (phase_ == Phase::Connected) { return kConnected; }
    return kDisconnected;
}

void Connector::start() {
    loop_->assertInLoopThread();
    if (phase_ != Phase::Idle) { return; }
    phase_ = Phase::StartQueued;
    const auto generation = ++generation_;
    loop_->queueInLoop([weak = weak_from_this(), generation] {
        if (auto self = weak.lock()) { self->startInLoop(generation); }
    });
}

void Connector::stop() {
    loop_->assertInLoopThread();
    ++generation_;
    phase_ = Phase::Idle;
    cancelTimers();
    if (channel_) { sockets::close(removeAndResetChannel()); }
}

void Connector::restart() {
    loop_->assertInLoopThread();
    stop();
    retryDelayMs_ = initialRetryDelay_;
    start();
}

void Connector::setRetryDelay(Duration initial, Duration max) {
    if (initial <= Duration::zero() || max < initial) {
        throw std::invalid_argument("Connector retry delays require 0 < initial <= max");
    }
    initialRetryDelay_ = initial;
    retryDelayMs_ = initial;
    maxRetryDelayMs_ = max;
}

void Connector::startInLoop(std::uint64_t generation) {
    loop_->assertInLoopThread();
    if (generation_ != generation || phase_ != Phase::StartQueued) { return; }
    phase_ = Phase::Connecting;
    emitEvent(ConnectorEvent::ConnectAttempt);
    // The hook may stop/restart, replace its callback or release the client.
    if (generation_ != generation || phase_ != Phase::Connecting) { return; }

    const auto sockfd = sockets::createNonblocking(serverAddr_.family());
    if (!sockets::isValid(sockfd)) {
        const int error = sockets::lastError();
        LOG_ERROR << "Connector::socket error: " << sockets::errorMessage(error);
        fail(kInvalidSocket, ConnectorEvent::ConnectFailed, generation,
             sockets::isConnectRetryable(error));
        return;
    }
    Socket socket(sockfd);
    const int ret = sockets::connect(socket.fd(), serverAddr_.getSockAddr(), serverAddr_.getSockAddrLen());
    const int savedError = (ret == 0) ? 0 : sockets::lastError();
    if (savedError == 0 || sockets::isInProgress(savedError) || sockets::isInterrupted(savedError)) {
        connecting(socket, generation);
        return;
    }

    LOG_ERROR << "Connector::connect error: " << sockets::errorMessage(savedError);
    fail(socket.releaseFd(), ConnectorEvent::ConnectFailed, generation,
         sockets::isConnectRetryable(savedError));
}

void Connector::connecting(Socket& socket, std::uint64_t generation) {
    assert(!channel_);
    // Keep fd ownership in Socket while allocations prepare the Channel. Only
    // the completed registration transfers it into the Connector lifecycle.
    auto channel = std::make_unique<Channel>(loop_, socket.fd());
    channel->tie(shared_from_this());
    const auto weak = weak_from_this();
    channel->setWriteCallback([weak, generation] {
        if (auto self = weak.lock()) { self->handleWrite(generation); }
    });
    channel->setErrorCallback([weak, generation] {
        if (auto self = weak.lock()) { self->handleError(generation); }
    });
    channel->enableWriting();
    channel_ = std::move(channel);
    socket.releaseFd();

    if (connectTimeout_ > Duration::zero()) {
        connectTimeoutTimerId_ = loop_->runAfter(connectTimeout_, [weak, generation] {
            if (auto self = weak.lock()) { self->handleConnectTimeout(generation); }
        });
    }
}

void Connector::handleWrite(std::uint64_t generation) {
    if (generation_ != generation || phase_ != Phase::Connecting) { return; }
    cancelTimers();
    Socket socket(removeAndResetChannel());
    const int err = sockets::getSocketError(socket.fd());
    if (err != 0) {
        LOG_ERROR << "Connector::handleWrite SO_ERROR = " << err << ": " << sockets::errorMessage(err);
        fail(socket.releaseFd(), ConnectorEvent::ConnectFailed, generation);
        return;
    }

    const sockaddr_storage localStorage = sockets::getLocalAddr(socket.fd());
    const sockaddr_storage peerStorage = sockets::getPeerAddr(socket.fd());
    bool selfConnect = false;
    if (localStorage.ss_family == peerStorage.ss_family) {
        if (localStorage.ss_family == AF_INET6) {
            const auto& local6 = *reinterpret_cast<const sockaddr_in6*>(&localStorage);
            const auto& peer6 = *reinterpret_cast<const sockaddr_in6*>(&peerStorage);
            selfConnect = (local6.sin6_port == peer6.sin6_port) &&
                          (std::memcmp(&local6.sin6_addr, &peer6.sin6_addr, sizeof(in6_addr)) == 0);
        } else {
            const auto& local4 = *reinterpret_cast<const sockaddr_in*>(&localStorage);
            const auto& peer4 = *reinterpret_cast<const sockaddr_in*>(&peerStorage);
            selfConnect = (local4.sin_port == peer4.sin_port) &&
                          (local4.sin_addr.s_addr == peer4.sin_addr.s_addr);
        }
    }
    if (selfConnect) {
        fail(socket.releaseFd(), ConnectorEvent::SelfConnectDetected, generation);
        return;
    }

    phase_ = Phase::Connected;
    emitEvent(ConnectorEvent::ConnectSuccess);
    if (generation_ != generation || phase_ != Phase::Connected) { return; }
    auto callback = newConnectionCallback_;
    if (callback) {
        callback(socket.releaseFd());
    } else {
        phase_ = Phase::Idle;
    }
}

void Connector::handleError(std::uint64_t generation) {
    if (generation_ != generation || phase_ != Phase::Connecting) { return; }
    const auto sockfd = removeAndResetChannel();
    const int err = sockets::getSocketError(sockfd);
    LOG_ERROR << "Connector::handleError SO_ERROR = " << err << ": " << sockets::errorMessage(err);
    fail(sockfd, ConnectorEvent::ConnectFailed, generation);
}

void Connector::handleConnectTimeout(std::uint64_t generation) {
    if (generation_ != generation || phase_ != Phase::Connecting) { return; }
    connectTimeoutTimerId_ = {};
    LOG_WARN << "Connector::handleConnectTimeout: connect to "
             << serverAddr_.toIpPort() << " timed out";
    fail(removeAndResetChannel(), ConnectorEvent::ConnectTimeout, generation);
}

void Connector::fail(SocketFd sockfd, ConnectorEvent event, std::uint64_t generation,
                     bool retryable) {
    cancelTimers();
    sockets::close(sockfd);
    phase_ = Phase::Idle;
    emitEvent(event);
    if (generation_ == generation && phase_ == Phase::Idle && retryEnabled_ && retryable) {
        scheduleRetry(generation);
    }
}

void Connector::scheduleRetry(std::uint64_t generation) {
    phase_ = Phase::RetryWaiting;
    retryTimerId_ = loop_->runAfter(retryDelayMs_, [weak = weak_from_this(), generation] {
        if (auto self = weak.lock()) {
            if (self->generation_ != generation || self->phase_ != Phase::RetryWaiting) { return; }
            self->retryTimerId_ = {};
            self->phase_ = Phase::Idle;
            self->start();
        }
    });
    // Saturating backoff avoids overflowing a duration close to its maximum.
    retryDelayMs_ = retryDelayMs_ > maxRetryDelayMs_ - retryDelayMs_
                        ? maxRetryDelayMs_ : retryDelayMs_ * 2;
    // Publish the installed timer before a reentrant hook can stop/restart.
    emitEvent(ConnectorEvent::RetryScheduled);
}

void Connector::cancelTimers() {
    if (retryTimerId_.valid()) {
        loop_->cancel(retryTimerId_);
        retryTimerId_ = {};
    }
    if (connectTimeoutTimerId_.valid()) {
        loop_->cancel(connectTimeoutTimerId_);
        connectTimeoutTimerId_ = {};
    }
}

void Connector::emitEvent(ConnectorEvent event) {
    auto callback = connectorEventCallback_;
    if (callback) { callback(serverAddr_, event); }
}

SocketFd Connector::removeAndResetChannel() {
    channel_->disableAll();
    channel_->remove();
    const SocketFd sockfd = channel_->fd();
    // The old Channel may still be on its event stack. Own that exact removed
    // instance independently: later retirement cannot reset a new channel_.
    std::shared_ptr<Channel> retired(std::move(channel_));
    loop_->queueInLoop([retired = std::move(retired)] {});
    return sockfd;
}

} // namespace mini::net
