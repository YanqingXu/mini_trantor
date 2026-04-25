#pragma once

// SocketsOps 暴露最底层的 socket 系统调用辅助函数。
// IPv4/IPv6 双栈：createNonblockingOrDie 接受 family 参数，
// bind/accept/getLocalAddr/getPeerAddr 统一使用 sockaddr_storage。

#include <netinet/in.h>
#include <sys/socket.h>

namespace mini::net::sockets {

/// Create a non-blocking, close-on-exec TCP socket for the given address family.
/// @param family  AF_INET or AF_INET6
int createNonblockingOrDie(sa_family_t family);

void bindOrDie(int sockfd, const sockaddr_storage& addr);
void listenOrDie(int sockfd);
int accept(int sockfd, sockaddr_storage* addr);
void close(int sockfd);
void shutdownWrite(int sockfd);
int getSocketError(int sockfd);
sockaddr_storage getLocalAddr(int sockfd);
sockaddr_storage getPeerAddr(int sockfd);

}  // namespace mini::net::sockets
