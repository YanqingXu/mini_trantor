#include "mini/net/Acceptor.h"

#include "mini/net/EventLoop.h"
#include "mini/net/SocketsOps.h"

#include "mini/base/Logger.h"

#include <cerrno>

namespace mini::net {

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reusePort)
    : loop_(loop),
      acceptSocket_(sockets::createNonblockingOrDie(listenAddr.family())),
      acceptChannel_(loop, acceptSocket_.fd()),
      listening_(false) {
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.setReusePort(reusePort);
    // For IPv6 sockets, default to dual-stack (IPV6_V6ONLY=0) so the same
    // socket can accept both IPv4-mapped and native IPv6 connections.
    if (listenAddr.isIpv6()) {
        acceptSocket_.setIpv6Only(false);
    }
    acceptSocket_.bindAddress(listenAddr);
    acceptChannel_.setReadCallback([this](mini::base::Timestamp receiveTime) { handleRead(receiveTime); });
}

Acceptor::~Acceptor() {
    if (!loop_->isInLoopThread()) {
        LOG_FATAL << "Acceptor destroyed from non-owner thread";
    }
    if (listening_) {
        acceptChannel_.disableAll();
        acceptChannel_.remove();
    }
}

void Acceptor::setNewConnectionCallback(NewConnectionCallback cb) {
    newConnectionCallback_ = std::move(cb);
}

bool Acceptor::listening() const noexcept {
    return listening_;
}

void Acceptor::listen() {
    loop_->assertInLoopThread();
    listening_ = true;
    acceptSocket_.listen();
    acceptChannel_.enableReading();
}

void Acceptor::stop() {
    loop_->assertInLoopThread();
    if (!listening_) {
        return;
    }
    listening_ = false;
    acceptChannel_.disableAll();
    acceptChannel_.remove();
    // Close the listen socket so the kernel rejects further connections.
    // Release the fd from the Socket RAII wrapper first to avoid double-close.
    const int fd = acceptSocket_.releaseFd();
    if (fd >= 0) {
        sockets::close(fd);
    }
}

void Acceptor::handleRead(mini::base::Timestamp receiveTime) {
    (void)receiveTime;
    loop_->assertInLoopThread();

    while (true) {
        InetAddress peerAddr;
        const int connfd = acceptSocket_.accept(&peerAddr);
        if (connfd >= 0) {
            if (newConnectionCallback_) {
                newConnectionCallback_(connfd, peerAddr);
            } else {
                sockets::close(connfd);
            }
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
}

}  // namespace mini::net
