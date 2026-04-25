#include "mini/net/SocketsOps.h"

#include "mini/base/Logger.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace mini::net::sockets {

namespace {

[[noreturn]] void die(const char* what) {
    LOG_SYSFATAL << what << ": " << std::strerror(errno);
    __builtin_unreachable();
}

}  // namespace

int createNonblockingOrDie(sa_family_t family) {
    const int sockfd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (sockfd < 0) {
        die("socket");
    }
    return sockfd;
}

void bindOrDie(int sockfd, const sockaddr_storage& addr) {
    socklen_t addrLen = 0;
    if (addr.ss_family == AF_INET6) {
        addrLen = static_cast<socklen_t>(sizeof(sockaddr_in6));
    } else {
        addrLen = static_cast<socklen_t>(sizeof(sockaddr_in));
    }
    if (::bind(sockfd, reinterpret_cast<const sockaddr*>(&addr), addrLen) < 0) {
        die("bind");
    }
}

void listenOrDie(int sockfd) {
    if (::listen(sockfd, SOMAXCONN) < 0) {
        die("listen");
    }
}

int accept(int sockfd, sockaddr_storage* addr) {
    socklen_t addrLen = static_cast<socklen_t>(sizeof(sockaddr_storage));
#ifdef __linux__
    return ::accept4(sockfd, reinterpret_cast<sockaddr*>(addr), &addrLen, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    const int connfd = ::accept(sockfd, reinterpret_cast<sockaddr*>(addr), &addrLen);
    if (connfd >= 0) {
        ::fcntl(connfd, F_SETFL, ::fcntl(connfd, F_GETFL, 0) | O_NONBLOCK);
        ::fcntl(connfd, F_SETFD, FD_CLOEXEC);
    }
    return connfd;
#endif
}

void close(int sockfd) {
    if (::close(sockfd) < 0) {
        LOG_SYSERR << "close: " << std::strerror(errno);
    }
}

void shutdownWrite(int sockfd) {
    if (::shutdown(sockfd, SHUT_WR) < 0) {
        LOG_SYSERR << "shutdown: " << std::strerror(errno);
    }
}

int getSocketError(int sockfd) {
    int optval = 0;
    socklen_t optlen = static_cast<socklen_t>(sizeof(optval));
    if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0) {
        return errno;
    }
    return optval;
}

sockaddr_storage getLocalAddr(int sockfd) {
    sockaddr_storage localAddr{};
    socklen_t addrLen = static_cast<socklen_t>(sizeof(localAddr));
    if (::getsockname(sockfd, reinterpret_cast<sockaddr*>(&localAddr), &addrLen) < 0) {
        die("getsockname");
    }
    return localAddr;
}

sockaddr_storage getPeerAddr(int sockfd) {
    sockaddr_storage peerAddr{};
    socklen_t addrLen = static_cast<socklen_t>(sizeof(peerAddr));
    if (::getpeername(sockfd, reinterpret_cast<sockaddr*>(&peerAddr), &addrLen) < 0) {
        std::memset(&peerAddr, 0, sizeof(peerAddr));
    }
    return peerAddr;
}

}  // namespace mini::net::sockets
