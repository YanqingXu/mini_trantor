#include "mini/net/Socket.h"

#include "mini/net/SocketsOps.h"

#include <cstring>
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

int Socket::releaseFd() noexcept {
    const int fd = sockfd_;
    sockfd_ = -1;
    return fd;
}

void Socket::bindAddress(const InetAddress& localAddr) {
    sockaddr_storage storage{};
    std::memcpy(&storage, localAddr.getSockAddr(), localAddr.getSockAddrLen());
    sockets::bindOrDie(sockfd_, storage);
}

void Socket::listen() {
    sockets::listenOrDie(sockfd_);
}

int Socket::accept(InetAddress* peerAddr) {
    sockaddr_storage addr{};
    const int connfd = sockets::accept(sockfd_, &addr);
    if (connfd >= 0 && peerAddr != nullptr) {
        if (addr.ss_family == AF_INET6) {
            peerAddr->setSockAddrInet6(*reinterpret_cast<const sockaddr_in6*>(&addr));
        } else {
            peerAddr->setSockAddrInet(*reinterpret_cast<const sockaddr_in*>(&addr));
        }
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

void Socket::setIpv6Only(bool on) {
    setSocketOption(sockfd_, IPPROTO_IPV6, IPV6_V6ONLY, on);
}

}  // namespace mini::net
