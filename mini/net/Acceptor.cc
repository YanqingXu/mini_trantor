#include "mini/net/Acceptor.h"

#include "mini/net/EventLoop.h"
#include "mini/net/SocketsOps.h"

#include "mini/base/Logger.h"

#include <cerrno>

namespace mini::net {

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reusePort)
    : loop_(loop),
      acceptSocket_(sockets::createNonblockingOrDie()),
      acceptChannel_(loop, acceptSocket_.fd()),
      listening_(false) {
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.setReusePort(reusePort);
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
