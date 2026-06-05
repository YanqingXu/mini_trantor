#pragma once

// InetAddress 是 IPv4/IPv6 双栈地址的轻量封装。
// 内部使用 sockaddr_storage 存储，family 字段显式区分协议族。
// 对外保持 IPv4 兼容 API (getSockAddrInet / setSockAddrInet)，
// 同时新增 IPv6 API (getSockAddrInet6 / setSockAddrInet6)，
// 以及泛型 API (getSockAddr / getSockAddrLen / sockaddr_storage 构造)。

#include <cstdint>
#include <netinet/in.h>
#include <string>

namespace mini::net {

class InetAddress {
public:
    // --- IPv4 constructors (backward compatible) ---
    explicit InetAddress(uint16_t port = 0, bool loopbackOnly = false);
    InetAddress(std::string ip, uint16_t port);
    explicit InetAddress(const sockaddr_in& addr);

    // --- IPv6 constructors ---
    /// Construct an IPv6 address from IP string and port.
    /// Accepts both bare ("::1") and bracketed ("[::1]") forms.
    /// If the string is an IPv4 address, falls back to IPv4 construction.
    InetAddress(std::string ip, uint16_t port, bool ipv6);

    /// Construct from a raw sockaddr_in6.
    explicit InetAddress(const sockaddr_in6& addr);

    /// Construct from a raw sockaddr_storage (family determined by ss_family).
    explicit InetAddress(const sockaddr_storage& storage);

    // --- IPv4 accessors (backward compatible) ---
    const sockaddr_in& getSockAddrInet() const noexcept;
    void setSockAddrInet(const sockaddr_in& addr) noexcept;

    // --- IPv6 accessors ---
    const sockaddr_in6& getSockAddrInet6() const noexcept;
    void setSockAddrInet6(const sockaddr_in6& addr) noexcept;

    // --- Generic accessors ---
    /// Return a const pointer to the underlying sockaddr (either IPv4 or IPv6).
    const sockaddr* getSockAddr() const noexcept;
    /// Return the size of the underlying sockaddr structure based on family.
    socklen_t getSockAddrLen() const noexcept;

    // --- Family query ---
    sa_family_t family() const noexcept;
    bool isIpv4() const noexcept;
    bool isIpv6() const noexcept;

    // --- Text conversion ---
    std::string toIp() const;
    /// IPv4: "ip:port"   IPv6: "[ip]:port"
    std::string toIpPort() const;
    uint16_t port() const noexcept;

private:
    sockaddr_storage addr_{};
    sa_family_t family_{AF_INET};
};

}  // namespace mini::net
