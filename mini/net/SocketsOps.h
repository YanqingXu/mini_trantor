#pragma once

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
