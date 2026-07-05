#include "mini/net/SocketsOps.h"

#include "mini/base/Logger.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mini::net::sockets {

namespace {

[[noreturn]] void die(const char* what) {
    LOG_SYSFATAL << what << ": " << errorMessage(lastError());
    std::abort();
}

#ifdef _WIN32
struct WinsockRuntime {
    WinsockRuntime() {
        WSADATA data{};
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0) {
            LOG_SYSFATAL << "WSAStartup: " << rc;
            std::abort();
        }
    }

    ~WinsockRuntime() {
        ::WSACleanup();
    }
};
#endif

void setNonBlockingOrDie(SocketFd sockfd) {
#ifdef _WIN32
    u_long on = 1;
    if (::ioctlsocket(sockfd, FIONBIO, &on) == SOCKET_ERROR) {
        die("ioctlsocket(FIONBIO)");
    }
#else
    const int flags = ::fcntl(sockfd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        die("fcntl(O_NONBLOCK)");
    }
    (void)::fcntl(sockfd, F_SETFD, FD_CLOEXEC);
#endif
}

}  // namespace

void ensureInitialized() {
#ifdef _WIN32
    static WinsockRuntime runtime;
    (void)runtime;
#endif
}

SocketFd createNonblockingOrDie(sa_family_t family) {
    ensureInitialized();
#ifdef _WIN32
    const SocketFd sockfd = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
#else
    const SocketFd sockfd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
#endif
    if (!isValid(sockfd)) {
        die("socket");
    }
#ifdef _WIN32
    setNonBlockingOrDie(sockfd);
#endif
    return sockfd;
}

SocketFd createNonblockingDatagramOrDie(sa_family_t family) {
    ensureInitialized();
#ifdef _WIN32
    const SocketFd sockfd = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
#else
    const SocketFd sockfd =
        ::socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
#endif
    if (!isValid(sockfd)) {
        die("socket");
    }
#ifdef _WIN32
    setNonBlockingOrDie(sockfd);
#endif
    return sockfd;
}

void bindOrDie(SocketFd sockfd, const sockaddr_storage& addr) {
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

void listenOrDie(SocketFd sockfd) {
    if (::listen(sockfd, SOMAXCONN) < 0) {
        die("listen");
    }
}

SocketFd accept(SocketFd sockfd, sockaddr_storage* addr) {
    socklen_t addrLen = static_cast<socklen_t>(sizeof(sockaddr_storage));
#ifdef __linux__
    return ::accept4(sockfd, reinterpret_cast<sockaddr*>(addr), &addrLen, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    const SocketFd connfd = ::accept(sockfd, reinterpret_cast<sockaddr*>(addr), &addrLen);
    if (isValid(connfd)) {
        setNonBlockingOrDie(connfd);
    }
    return connfd;
#endif
}

int connect(SocketFd sockfd, const sockaddr* addr, socklen_t addrLen) {
    return ::connect(sockfd, addr, addrLen);
}

void close(SocketFd sockfd) {
    if (!isValid(sockfd)) {
        return;
    }
#ifdef _WIN32
    if (::closesocket(sockfd) == SOCKET_ERROR) {
        LOG_SYSERR << "closesocket: " << errorMessage(lastError());
    }
#else
    if (::close(sockfd) < 0) {
        LOG_SYSERR << "close: " << std::strerror(errno);
    }
#endif
}

void shutdownWrite(SocketFd sockfd) {
#ifdef _WIN32
    if (::shutdown(sockfd, SD_SEND) == SOCKET_ERROR) {
        LOG_SYSERR << "shutdown: " << errorMessage(lastError());
    }
#else
    if (::shutdown(sockfd, SHUT_WR) < 0) {
        LOG_SYSERR << "shutdown: " << std::strerror(errno);
    }
#endif
}

int getSocketError(SocketFd sockfd) {
    int optval = 0;
    socklen_t optlen = static_cast<socklen_t>(sizeof(optval));
    if (::getsockopt(
            sockfd,
            SOL_SOCKET,
            SO_ERROR,
            reinterpret_cast<char*>(&optval),
            &optlen) < 0) {
        return lastError();
    }
    return optval;
}

sockaddr_storage getLocalAddr(SocketFd sockfd) {
    sockaddr_storage localAddr{};
    socklen_t addrLen = static_cast<socklen_t>(sizeof(localAddr));
    if (::getsockname(sockfd, reinterpret_cast<sockaddr*>(&localAddr), &addrLen) < 0) {
        die("getsockname");
    }
    return localAddr;
}

sockaddr_storage getPeerAddr(SocketFd sockfd) {
    sockaddr_storage peerAddr{};
    socklen_t addrLen = static_cast<socklen_t>(sizeof(peerAddr));
    if (::getpeername(sockfd, reinterpret_cast<sockaddr*>(&peerAddr), &addrLen) < 0) {
        std::memset(&peerAddr, 0, sizeof(peerAddr));
    }
    return peerAddr;
}

ssize_t read(SocketFd sockfd, void* buffer, std::size_t len) {
#ifdef _WIN32
    const auto capped = static_cast<int>(std::min<std::size_t>(
        len,
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    const int n = ::recv(sockfd, static_cast<char*>(buffer), capped, 0);
    return n == SOCKET_ERROR ? -1 : static_cast<ssize_t>(n);
#else
    return ::read(sockfd, buffer, len);
#endif
}

ssize_t write(SocketFd sockfd, const void* buffer, std::size_t len) {
#ifdef _WIN32
    const auto capped = static_cast<int>(std::min<std::size_t>(
        len,
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    const int n = ::send(sockfd, static_cast<const char*>(buffer), capped, 0);
    return n == SOCKET_ERROR ? -1 : static_cast<ssize_t>(n);
#else
    return ::write(sockfd, buffer, len);
#endif
}

ssize_t recvFrom(SocketFd sockfd, void* buffer, std::size_t len, sockaddr_storage* addr, socklen_t* addrLen) {
#ifdef _WIN32
    const auto capped = static_cast<int>(std::min<std::size_t>(
        len,
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    const int n = ::recvfrom(
        sockfd,
        static_cast<char*>(buffer),
        capped,
        0,
        reinterpret_cast<sockaddr*>(addr),
        addrLen);
    return n == SOCKET_ERROR ? -1 : static_cast<ssize_t>(n);
#else
    return ::recvfrom(sockfd, buffer, len, 0, reinterpret_cast<sockaddr*>(addr), addrLen);
#endif
}

ssize_t sendTo(SocketFd sockfd, const void* data, std::size_t len, const sockaddr* addr, socklen_t addrLen) {
#ifdef _WIN32
    const auto capped = static_cast<int>(std::min<std::size_t>(
        len,
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    const int n = ::sendto(sockfd, static_cast<const char*>(data), capped, 0, addr, addrLen);
    return n == SOCKET_ERROR ? -1 : static_cast<ssize_t>(n);
#else
    return ::sendto(sockfd, data, len, 0, addr, addrLen);
#endif
}

int lastError() noexcept {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

void setLastError(int error) noexcept {
#ifdef _WIN32
    ::WSASetLastError(error);
#else
    errno = error;
#endif
}

bool isValid(SocketFd sockfd) noexcept {
    return sockfd != kInvalidSocket;
}

bool isWouldBlock(int error) noexcept {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

bool isInterrupted(int error) noexcept {
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

bool isInProgress(int error) noexcept {
#ifdef _WIN32
    return error == WSAEINPROGRESS || error == WSAEWOULDBLOCK || error == WSAEISCONN;
#else
    return error == EINPROGRESS || error == EISCONN;
#endif
}

bool isConnectRetryable(int error) noexcept {
#ifdef _WIN32
    return error == WSAEADDRINUSE ||
           error == WSAEADDRNOTAVAIL ||
           error == WSAECONNREFUSED ||
           error == WSAENETUNREACH ||
           error == WSAETIMEDOUT;
#else
    return error == EAGAIN ||
           error == EADDRINUSE ||
           error == EADDRNOTAVAIL ||
           error == ECONNREFUSED ||
           error == ENETUNREACH;
#endif
}

bool isMessageSize(int error) noexcept {
#ifdef _WIN32
    return error == WSAEMSGSIZE;
#else
    return error == EMSGSIZE;
#endif
}

std::string errorMessage(int error) {
#ifdef _WIN32
    char* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = ::FormatMessageA(
        flags,
        nullptr,
        static_cast<DWORD>(error),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&message),
        0,
        nullptr);
    if (length == 0 || message == nullptr) {
        return "WinSock error " + std::to_string(error);
    }
    std::string result(message, length);
    ::LocalFree(message);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
#else
    return std::strerror(error);
#endif
}

#ifdef _WIN32
void createSocketPairOrDie(SocketFd fds[2]) {
    ensureInitialized();

    SocketFd listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!isValid(listener)) {
        die("socketpair listener socket");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        ::listen(listener, 1) == SOCKET_ERROR) {
        close(listener);
        die("socketpair bind/listen");
    }

    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) {
        close(listener);
        die("socketpair getsockname");
    }

    SocketFd writer = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!isValid(writer)) {
        close(listener);
        die("socketpair writer socket");
    }

    if (::connect(writer, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        close(writer);
        close(listener);
        die("socketpair connect");
    }

    SocketFd reader = ::accept(listener, nullptr, nullptr);
    if (!isValid(reader)) {
        close(writer);
        close(listener);
        die("socketpair accept");
    }

    close(listener);
    setNonBlockingOrDie(reader);
    setNonBlockingOrDie(writer);
    fds[0] = reader;
    fds[1] = writer;
}
#endif

}  // namespace mini::net::sockets
