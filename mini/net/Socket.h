#pragma once

#include "mini/base/noncopyable.h"
#include "mini/net/InetAddress.h"

namespace mini::net {

class Socket : private mini::base::noncopyable {
public:
    explicit Socket(int sockfd);
    ~Socket();

    int fd() const noexcept;

    void bindAddress(const InetAddress& localAddr);
    void listen();
    int accept(InetAddress* peerAddr);
    void shutdownWrite();

    void setReuseAddr(bool on);
    void setReusePort(bool on);
    void setKeepAlive(bool on);
    void setTcpNoDelay(bool on);

private:
    int sockfd_;
};

}  // namespace mini::net
