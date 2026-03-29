#pragma once

// InetAddress 是 IPv4 地址与端口的轻量封装。
// 它只负责 sockaddr_in 的构造、保存与文本转换。

#include <cstdint>
#include <netinet/in.h>
#include <string>

namespace mini::net {

class InetAddress {
public:
    explicit InetAddress(uint16_t port = 0, bool loopbackOnly = false);
    InetAddress(std::string ip, uint16_t port);
    explicit InetAddress(const sockaddr_in& addr);

    const sockaddr_in& getSockAddrInet() const noexcept;
    void setSockAddrInet(const sockaddr_in& addr) noexcept;

    std::string toIp() const;
    std::string toIpPort() const;
    uint16_t port() const noexcept;

private:
    sockaddr_in addr_{};
};

}  // namespace mini::net
