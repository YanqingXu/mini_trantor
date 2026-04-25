#pragma once

// Socket 是对 fd 级 socket 操作的薄 RAII 封装。
// 它负责 listen/accept/shutdown/setsockopt 之类的系统调用边界。

#include "mini/base/noncopyable.h"
#include "mini/net/InetAddress.h"

namespace mini::net {

class Socket : private mini::base::noncopyable {
public:
    explicit Socket(int sockfd);
    ~Socket();

    int fd() const noexcept;

    /// Release ownership of the socket fd.  After this call, the Socket
    /// destructor will NOT close the fd.  Returns the released fd.
    int releaseFd() noexcept;

    void bindAddress(const InetAddress& localAddr);
    void listen();
    int accept(InetAddress* peerAddr);
    void shutdownWrite();

    void setReuseAddr(bool on);
    void setReusePort(bool on);
    void setKeepAlive(bool on);
    void setTcpNoDelay(bool on);
    void setIpv6Only(bool on);

private:
    int sockfd_;
};

}  // namespace mini::net
