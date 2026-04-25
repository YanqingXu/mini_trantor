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
    : loop_(loop),
      serverAddr_(serverAddr),
      state_(kDisconnected),
      connect_(false),
      retryDelayMs_(kDefaultInitRetryDelay),
      maxRetryDelayMs_(kDefaultMaxRetryDelay) {
}

Connector::~Connector() {
    // Cancel any pending retry timer.
    if (retryTimerId_.valid()) {
        loop_->cancel(retryTimerId_);
        retryTimerId_ = {};
    }
    // Channel should already be removed before destruction.
    assert(!channel_);
}

void Connector::setNewConnectionCallback(NewConnectionCallback cb) {
    newConnectionCallback_ = std::move(cb);
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
    if (state_ == kConnecting) {
        state_ = kDisconnected;
        const int sockfd = removeAndResetChannel();
        sockets::close(sockfd);
    }
}

void Connector::connect() {
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
}

void Connector::handleWrite() {
    if (state_ != kConnecting) {
        return;
    }

    // Remove channel before delivering fd — ownership transfers to upper layer.
    const int sockfd = removeAndResetChannel();

    const int err = sockets::getSocketError(sockfd);
    if (err != 0) {
        LOG_ERROR << "Connector::handleWrite SO_ERROR = " << err << ": " << std::strerror(err);
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
        retry(sockfd);
        return;
    }

    state_ = kConnected;
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
    const int sockfd = removeAndResetChannel();
    const int err = sockets::getSocketError(sockfd);
    LOG_ERROR << "Connector::handleError SO_ERROR = " << err << ": " << std::strerror(err);
    retry(sockfd);
}

void Connector::retry(int sockfd) {
    sockets::close(sockfd);
    state_ = kDisconnected;
    if (connect_) {
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
