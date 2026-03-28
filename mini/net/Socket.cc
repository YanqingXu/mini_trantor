#include "mini/net/Socket.h"

#include "mini/net/SocketsOps.h"

#include <netinet/tcp.h>

namespace mini::net {

namespace {

void setSocketOption(int sockfd, int level, int option, bool on) {
    const int optval = on ? 1 : 0;
    ::setsockopt(sockfd, level, option, &optval, static_cast<socklen_t>(sizeof(optval)));
}

}  // namespace

Socket::Socket(int sockfd) : sockfd_(sockfd) {
}

Socket::~Socket() {
    sockets::close(sockfd_);
}

int Socket::fd() const noexcept {
    return sockfd_;
}

void Socket::bindAddress(const InetAddress& localAddr) {
    sockets::bindOrDie(sockfd_, localAddr.getSockAddrInet());
}

void Socket::listen() {
    sockets::listenOrDie(sockfd_);
}

int Socket::accept(InetAddress* peerAddr) {
    sockaddr_in addr{};
    const int connfd = sockets::accept(sockfd_, &addr);
    if (connfd >= 0 && peerAddr != nullptr) {
        peerAddr->setSockAddrInet(addr);
    }
    return connfd;
}

void Socket::shutdownWrite() {
    sockets::shutdownWrite(sockfd_);
}

void Socket::setReuseAddr(bool on) {
    setSocketOption(sockfd_, SOL_SOCKET, SO_REUSEADDR, on);
}

void Socket::setReusePort(bool on) {
#ifdef SO_REUSEPORT
    setSocketOption(sockfd_, SOL_SOCKET, SO_REUSEPORT, on);
#else
    (void)on;
#endif
}

void Socket::setKeepAlive(bool on) {
    setSocketOption(sockfd_, SOL_SOCKET, SO_KEEPALIVE, on);
}

void Socket::setTcpNoDelay(bool on) {
    setSocketOption(sockfd_, IPPROTO_TCP, TCP_NODELAY, on);
}

}  // namespace mini::net
