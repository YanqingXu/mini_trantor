#pragma once

// SocketsOps 暴露最底层的 socket 系统调用辅助函数。
// 这里保持 C 风格边界，供上层 Socket/Acceptor/TcpConnection 复用。

#include <netinet/in.h>
#include <sys/socket.h>

namespace mini::net::sockets {

int createNonblockingOrDie();
void bindOrDie(int sockfd, const sockaddr_in& addr);
void listenOrDie(int sockfd);
int accept(int sockfd, sockaddr_in* addr);
void close(int sockfd);
void shutdownWrite(int sockfd);
int getSocketError(int sockfd);
sockaddr_in getLocalAddr(int sockfd);
sockaddr_in getPeerAddr(int sockfd);

}  // namespace mini::net::sockets
