#pragma once

// SocketsOps 暴露最底层的 socket 系统调用辅助函数。
// IPv4/IPv6 双栈：createNonblocking 返回可恢复的创建失败，OrDie 入口保留 fail-fast，
// bind/accept/getLocalAddr/getPeerAddr 统一使用 sockaddr_storage。

#include "mini/net/SocketTypes.h"

#include <cstddef>
#include <string>

namespace mini::net::sockets {

void ensureInitialized();

/// Create a non-blocking TCP socket (also close-on-exec on Linux).
/// The caller owns a successful result. On failure, returns kInvalidSocket and
/// preserves the platform error in lastError() on the calling thread.
/// Windows process-wide WinSock initialization retains its fail-fast policy.
/// @param family  AF_INET or AF_INET6
SocketFd createNonblocking(sa_family_t family);

/// Create a non-blocking TCP socket, terminating on creation/setup failure.
SocketFd createNonblockingOrDie(sa_family_t family);
SocketFd createNonblockingDatagramOrDie(sa_family_t family);

void bindOrDie(SocketFd sockfd, const sockaddr_storage& addr);
void listenOrDie(SocketFd sockfd);
SocketFd accept(SocketFd sockfd, sockaddr_storage* addr);
int connect(SocketFd sockfd, const sockaddr* addr, socklen_t addrLen);
void close(SocketFd sockfd);
void shutdownWrite(SocketFd sockfd);
int getSocketError(SocketFd sockfd);
sockaddr_storage getLocalAddr(SocketFd sockfd);
sockaddr_storage getPeerAddr(SocketFd sockfd);

ssize_t read(SocketFd sockfd, void* buffer, std::size_t len);
ssize_t write(SocketFd sockfd, const void* buffer, std::size_t len);
ssize_t recvFrom(SocketFd sockfd, void* buffer, std::size_t len, sockaddr_storage* addr, socklen_t* addrLen);
ssize_t sendTo(SocketFd sockfd, const void* data, std::size_t len, const sockaddr* addr, socklen_t addrLen);

int lastError() noexcept;
void setLastError(int error) noexcept;
bool isValid(SocketFd sockfd) noexcept;
bool isWouldBlock(int error) noexcept;
bool isInterrupted(int error) noexcept;
bool isInProgress(int error) noexcept;
bool isConnectRetryable(int error) noexcept;
bool isMessageSize(int error) noexcept;
std::string errorMessage(int error);

#ifdef _WIN32
void createSocketPairOrDie(SocketFd fds[2]);
#endif

}  // namespace mini::net::sockets
