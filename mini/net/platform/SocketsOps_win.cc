#include "mini/net/SocketsOps.h"

#include "mini/base/Logger.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace mini::net::sockets {

namespace {

[[noreturn]] void die(const char* what) {
    LOG_SYSFATAL << what << ": " << errorMessage(lastError());
    std::abort();
}

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

void setNonBlockingOrDie(SocketFd sockfd) {
    u_long on = 1;
    if (::ioctlsocket(sockfd, FIONBIO, &on) == SOCKET_ERROR) {
        die("ioctlsocket(FIONBIO)");
    }
}

}  // namespace

void ensureInitialized() {
    static WinsockRuntime runtime;
    (void)runtime;
}

SocketFd createNonblockingOrDie(sa_family_t family) {
    ensureInitialized();
    const SocketFd sockfd = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (!isValid(sockfd)) {
        die("socket");
    }
    setNonBlockingOrDie(sockfd);
    return sockfd;
}

SocketFd createNonblockingDatagramOrDie(sa_family_t family) {
    ensureInitialized();
    const SocketFd sockfd = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (!isValid(sockfd)) {
        die("socket");
    }
    setNonBlockingOrDie(sockfd);
    return sockfd;
}

void bindOrDie(SocketFd sockfd, const sockaddr_storage& addr) {
    socklen_t addrLen = 0;
    if (addr.ss_family == AF_INET6) {
        addrLen = static_cast<socklen_t>(sizeof(sockaddr_in6));
    } else {
        addrLen = static_cast<socklen_t>(sizeof(sockaddr_in));
    }
    if (::bind(sockfd, reinterpret_cast<const sockaddr*>(&addr), addrLen) == SOCKET_ERROR) {
        die("bind");
    }
}

void listenOrDie(SocketFd sockfd) {
    if (::listen(sockfd, SOMAXCONN) == SOCKET_ERROR) {
        die("listen");
    }
}

SocketFd accept(SocketFd sockfd, sockaddr_storage* addr) {
    socklen_t addrLen = static_cast<socklen_t>(sizeof(sockaddr_storage));
    const SocketFd connfd = ::accept(sockfd, reinterpret_cast<sockaddr*>(addr), &addrLen);
    if (isValid(connfd)) {
        setNonBlockingOrDie(connfd);
    }
    return connfd;
}

int connect(SocketFd sockfd, const sockaddr* addr, socklen_t addrLen) {
    return ::connect(sockfd, addr, addrLen);
}

void close(SocketFd sockfd) {
    if (!isValid(sockfd)) {
        return;
    }
    if (::closesocket(sockfd) == SOCKET_ERROR) {
        LOG_SYSERR << "closesocket: " << errorMessage(lastError());
    }
}

void shutdownWrite(SocketFd sockfd) {
    if (::shutdown(sockfd, SD_SEND) == SOCKET_ERROR) {
        LOG_SYSERR << "shutdown: " << errorMessage(lastError());
    }
}

int getSocketError(SocketFd sockfd) {
    int optval = 0;
    socklen_t optlen = static_cast<socklen_t>(sizeof(optval));
    if (::getsockopt(
            sockfd,
            SOL_SOCKET,
            SO_ERROR,
            reinterpret_cast<char*>(&optval),
            &optlen) == SOCKET_ERROR) {
        return lastError();
    }
    return optval;
}

sockaddr_storage getLocalAddr(SocketFd sockfd) {
    sockaddr_storage localAddr{};
    socklen_t addrLen = static_cast<socklen_t>(sizeof(localAddr));
    if (::getsockname(sockfd, reinterpret_cast<sockaddr*>(&localAddr), &addrLen) == SOCKET_ERROR) {
        die("getsockname");
    }
    return localAddr;
}

sockaddr_storage getPeerAddr(SocketFd sockfd) {
    sockaddr_storage peerAddr{};
    socklen_t addrLen = static_cast<socklen_t>(sizeof(peerAddr));
    if (::getpeername(sockfd, reinterpret_cast<sockaddr*>(&peerAddr), &addrLen) == SOCKET_ERROR) {
        std::memset(&peerAddr, 0, sizeof(peerAddr));
    }
    return peerAddr;
}

ssize_t read(SocketFd sockfd, void* buffer, std::size_t len) {
    const auto capped = static_cast<int>(std::min<std::size_t>(
        len,
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    const int n = ::recv(sockfd, static_cast<char*>(buffer), capped, 0);
    return n == SOCKET_ERROR ? -1 : static_cast<ssize_t>(n);
}

ssize_t write(SocketFd sockfd, const void* buffer, std::size_t len) {
    const auto capped = static_cast<int>(std::min<std::size_t>(
        len,
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    const int n = ::send(sockfd, static_cast<const char*>(buffer), capped, 0);
    return n == SOCKET_ERROR ? -1 : static_cast<ssize_t>(n);
}

ssize_t recvFrom(SocketFd sockfd, void* buffer, std::size_t len, sockaddr_storage* addr, socklen_t* addrLen) {
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
}

ssize_t sendTo(SocketFd sockfd, const void* data, std::size_t len, const sockaddr* addr, socklen_t addrLen) {
    const auto capped = static_cast<int>(std::min<std::size_t>(
        len,
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    const int n = ::sendto(sockfd, static_cast<const char*>(data), capped, 0, addr, addrLen);
    return n == SOCKET_ERROR ? -1 : static_cast<ssize_t>(n);
}

int lastError() noexcept {
    return ::WSAGetLastError();
}

void setLastError(int error) noexcept {
    ::WSASetLastError(error);
}

bool isValid(SocketFd sockfd) noexcept {
    return sockfd != kInvalidSocket;
}

bool isWouldBlock(int error) noexcept {
    return error == WSAEWOULDBLOCK;
}

bool isInterrupted(int error) noexcept {
    return error == WSAEINTR;
}

bool isInProgress(int error) noexcept {
    return error == WSAEINPROGRESS || error == WSAEWOULDBLOCK || error == WSAEISCONN;
}

bool isConnectRetryable(int error) noexcept {
    return error == WSAEADDRINUSE ||
           error == WSAEADDRNOTAVAIL ||
           error == WSAECONNREFUSED ||
           error == WSAENETUNREACH ||
           error == WSAETIMEDOUT;
}

bool isMessageSize(int error) noexcept {
    return error == WSAEMSGSIZE;
}

std::string errorMessage(int error) {
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
}

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

}  // namespace mini::net::sockets
