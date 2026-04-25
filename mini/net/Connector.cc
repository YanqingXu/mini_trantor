#include "mini/net/Connector.h"

#include "mini/net/Channel.h"
#include "mini/net/EventLoop.h"
#include "mini/net/SocketsOps.h"

#include "mini/base/Logger.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>

namespace mini::net {

Connector::Connector(EventLoop* loop, const InetAddress& serverAddr)
    : Connector(loop, serverAddr, ConnectorOptions{}) {
}

Connector::Connector(EventLoop* loop, const InetAddress& serverAddr, ConnectorOptions options)
    : loop_(loop),
      serverAddr_(serverAddr),
      state_(kDisconnected),
      connect_(options.enableRetry),
      retryDelayMs_(options.initRetryDelay),
      maxRetryDelayMs_(options.maxRetryDelay),
      connectTimeout_(options.connectTimeout) {
}

Connector::~Connector() {
    // Cancel any pending retry timer.
    if (retryTimerId_.valid()) {
        loop_->cancel(retryTimerId_);
        retryTimerId_ = {};
    }
    // Cancel any pending connect timeout timer.
    if (connectTimeoutTimerId_.valid()) {
        loop_->cancel(connectTimeoutTimerId_);
        connectTimeoutTimerId_ = {};
    }
    // Channel should already be removed before destruction.
    assert(!channel_);
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
    return state_;
}

void Connector::start() {
    connect_ = true;
    loop_->runInLoop([self = shared_from_this()] { self->startInLoop(); });
}

void Connector::stop() {
    connect_ = false;
    loop_->runInLoop([self = shared_from_this()] { self->stopInLoop(); });
}

void Connector::restart() {
    loop_->assertInLoopThread();
    state_ = kDisconnected;
    retryDelayMs_ = kDefaultInitRetryDelay;
    connect_ = true;
    startInLoop();
}

void Connector::setRetryDelay(Duration initial, Duration max) {
    retryDelayMs_ = initial;
    maxRetryDelayMs_ = max;
}

void Connector::startInLoop() {
    loop_->assertInLoopThread();
    if (!connect_) {
        return;
    }
    assert(state_ == kDisconnected);
    connect();
}

void Connector::stopInLoop() {
    loop_->assertInLoopThread();
    // Cancel pending retry timer.
    if (retryTimerId_.valid()) {
        loop_->cancel(retryTimerId_);
        retryTimerId_ = {};
    }
    // Cancel pending connect timeout timer.
    if (connectTimeoutTimerId_.valid()) {
        loop_->cancel(connectTimeoutTimerId_);
        connectTimeoutTimerId_ = {};
    }
    if (state_ == kConnecting) {
        state_ = kDisconnected;
        const int sockfd = removeAndResetChannel();
        sockets::close(sockfd);
    }
}

void Connector::connect() {
    if (connectorEventCallback_) {
        connectorEventCallback_(serverAddr_, ConnectorEvent::ConnectAttempt);
    }

    const int sockfd = sockets::createNonblockingOrDie(serverAddr_.family());
    const int ret = ::connect(sockfd, serverAddr_.getSockAddr(), serverAddr_.getSockAddrLen());
    const int savedErrno = (ret == 0) ? 0 : errno;

    switch (savedErrno) {
        case 0:
        case EINPROGRESS:
        case EINTR:
        case EISCONN:
            connecting(sockfd);
            break;

        case EAGAIN:
        case EADDRINUSE:
        case EADDRNOTAVAIL:
        case ECONNREFUSED:
        case ENETUNREACH:
            if (connectorEventCallback_) {
                connectorEventCallback_(serverAddr_, ConnectorEvent::ConnectFailed);
            }
            retry(sockfd);
            break;

        case EACCES:
        case EPERM:
        case EAFNOSUPPORT:
        case EALREADY:
        case EBADF:
        case EFAULT:
        case ENOTSOCK:
        default:
            LOG_ERROR << "Connector::connect error: " << std::strerror(savedErrno);
            if (connectorEventCallback_) {
                connectorEventCallback_(serverAddr_, ConnectorEvent::ConnectFailed);
            }
            sockets::close(sockfd);
            break;
    }
}

void Connector::connecting(int sockfd) {
    state_ = kConnecting;
    assert(!channel_);
    channel_ = std::make_unique<Channel>(loop_, sockfd);
    channel_->setWriteCallback([this] { handleWrite(); });
    channel_->setErrorCallback([this] { handleError(); });
    channel_->enableWriting();

    // Register connect timeout timer if configured.
    if (connectTimeout_ > Duration::zero()) {
        connectTimeoutTimerId_ = loop_->runAfter(connectTimeout_, [self = shared_from_this()] {
            self->handleConnectTimeout();
        });
    }
}

void Connector::handleWrite() {
    if (state_ != kConnecting) {
        return;
    }

    // Cancel connect timeout timer on success path.
    if (connectTimeoutTimerId_.valid()) {
        loop_->cancel(connectTimeoutTimerId_);
        connectTimeoutTimerId_ = {};
    }

    // Remove channel before delivering fd — ownership transfers to upper layer.
    const int sockfd = removeAndResetChannel();

    const int err = sockets::getSocketError(sockfd);
    if (err != 0) {
        LOG_ERROR << "Connector::handleWrite SO_ERROR = " << err << ": " << std::strerror(err);
        if (connectorEventCallback_) {
            connectorEventCallback_(serverAddr_, ConnectorEvent::ConnectFailed);
        }
        retry(sockfd);
        return;
    }

    // Self-connect detection: compare local and peer addresses.
    const sockaddr_storage localStorage = sockets::getLocalAddr(sockfd);
    const sockaddr_storage peerStorage = sockets::getPeerAddr(sockfd);

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
        LOG_WARN << "Connector::handleWrite self-connect detected, retrying";
        if (connectorEventCallback_) {
            connectorEventCallback_(serverAddr_, ConnectorEvent::SelfConnectDetected);
        }
        retry(sockfd);
        return;
    }

    state_ = kConnected;
    if (connectorEventCallback_) {
        connectorEventCallback_(serverAddr_, ConnectorEvent::ConnectSuccess);
    }
    if (connect_ && newConnectionCallback_) {
        newConnectionCallback_(sockfd);
    } else {
        sockets::close(sockfd);
    }
}

void Connector::handleError() {
    if (state_ != kConnecting) {
        return;
    }

    // Cancel connect timeout timer.
    if (connectTimeoutTimerId_.valid()) {
        loop_->cancel(connectTimeoutTimerId_);
        connectTimeoutTimerId_ = {};
    }

    const int sockfd = removeAndResetChannel();
    const int err = sockets::getSocketError(sockfd);
    LOG_ERROR << "Connector::handleError SO_ERROR = " << err << ": " << std::strerror(err);
    if (connectorEventCallback_) {
        connectorEventCallback_(serverAddr_, ConnectorEvent::ConnectFailed);
    }
    retry(sockfd);
}

void Connector::handleConnectTimeout() {
    connectTimeoutTimerId_ = {};
    if (state_ != kConnecting) {
        // Connection already completed (success or failure) before timeout.
        return;
    }

    LOG_WARN << "Connector::handleConnectTimeout: connect to "
             << serverAddr_.toIpPort() << " timed out";

    if (connectorEventCallback_) {
        connectorEventCallback_(serverAddr_, ConnectorEvent::ConnectTimeout);
    }

    // Close the connecting socket and retry or fail.
    const int sockfd = removeAndResetChannel();
    sockets::close(sockfd);
    state_ = kDisconnected;

    if (connect_) {
        retry(0);  // No socket to close (already closed), but schedule retry.
    }
}

void Connector::retry(int sockfd) {
    if (sockfd >= 0) {
        sockets::close(sockfd);
    }
    state_ = kDisconnected;
    if (connect_) {
        if (connectorEventCallback_) {
            connectorEventCallback_(serverAddr_, ConnectorEvent::RetryScheduled);
        }
        // Schedule retry with backoff via EventLoop timer.
        retryTimerId_ = loop_->runAfter(retryDelayMs_, [self = shared_from_this()] {
            self->retryTimerId_ = {};
            self->startInLoop();
        });
        // Exponential backoff: double the delay up to max.
        retryDelayMs_ = std::min(retryDelayMs_ * 2, maxRetryDelayMs_);
    }
}

int Connector::removeAndResetChannel() {
    channel_->disableAll();
    channel_->remove();
    const int sockfd = channel_->fd();
    // Can't reset channel_ here because we're inside a channel callback.
    // Defer the reset via queueInLoop.
    loop_->queueInLoop([self = shared_from_this()] { self->resetChannel(); });
    return sockfd;
}

void Connector::resetChannel() {
    channel_.reset();
}

}  // namespace mini::net
