#include "mini/net/InetAddress.h"

#include <arpa/inet.h>
#include <cstring>
#include <stdexcept>

namespace mini::net {

// --- IPv4 constructors ---

InetAddress::InetAddress(uint16_t port, bool loopbackOnly) {
    std::memset(&addr_, 0, sizeof(addr_));
    auto* addr4 = reinterpret_cast<sockaddr_in*>(&addr_);
    addr4->sin_family = AF_INET;
    addr4->sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    addr4->sin_port = htons(port);
    family_ = AF_INET;
}

InetAddress::InetAddress(std::string ip, uint16_t port) {
    std::memset(&addr_, 0, sizeof(addr_));
    auto* addr4 = reinterpret_cast<sockaddr_in*>(&addr_);
    addr4->sin_family = AF_INET;
    addr4->sin_addr.s_addr = htonl(INADDR_ANY);
    addr4->sin_port = htons(port);
    family_ = AF_INET;

    if (::inet_pton(AF_INET, ip.c_str(), &addr4->sin_addr) == 1) {
        return;
    }

    // Try IPv6 if IPv4 parse fails — strip bracket notation first.
    std::string ipStr = ip;
    if (ipStr.size() >= 2 && ipStr.front() == '[' && ipStr.back() == ']') {
        ipStr = ipStr.substr(1, ipStr.size() - 2);
    }

    auto* addr6 = reinterpret_cast<sockaddr_in6*>(&addr_);
    std::memset(&addr_, 0, sizeof(addr_));
    addr6->sin6_family = AF_INET6;
    addr6->sin6_addr = in6addr_any;
    addr6->sin6_port = htons(port);
    family_ = AF_INET6;

    if (::inet_pton(AF_INET6, ipStr.c_str(), &addr6->sin6_addr) <= 0) {
        throw std::runtime_error("invalid IP address: " + ip);
    }
}

InetAddress::InetAddress(const sockaddr_in& addr) {
    std::memset(&addr_, 0, sizeof(addr_));
    *reinterpret_cast<sockaddr_in*>(&addr_) = addr;
    family_ = AF_INET;
}

// --- IPv6 constructors ---

InetAddress::InetAddress(std::string ip, uint16_t port, bool ipv6) {
    std::memset(&addr_, 0, sizeof(addr_));
    auto* addr6 = reinterpret_cast<sockaddr_in6*>(&addr_);
    addr6->sin6_family = AF_INET6;
    addr6->sin6_addr = in6addr_any;
    addr6->sin6_port = htons(port);
    family_ = AF_INET6;

    // Strip bracket notation if present: [::1] → ::1
    std::string ipStr = ip;
    if (ipStr.size() >= 2 && ipStr.front() == '[' && ipStr.back() == ']') {
        ipStr = ipStr.substr(1, ipStr.size() - 2);
    }

    if (::inet_pton(AF_INET6, ipStr.c_str(), &addr6->sin6_addr) <= 0) {
        throw std::runtime_error("invalid IPv6 address: " + ip);
    }
}

InetAddress::InetAddress(const sockaddr_in6& addr) {
    std::memset(&addr_, 0, sizeof(addr_));
    *reinterpret_cast<sockaddr_in6*>(&addr_) = addr;
    family_ = AF_INET6;
}

InetAddress::InetAddress(const sockaddr_storage& storage) {
    std::memset(&addr_, 0, sizeof(addr_));
    addr_ = storage;
    family_ = storage.ss_family;
}

// --- IPv4 accessors ---

const sockaddr_in& InetAddress::getSockAddrInet() const noexcept {
    return *reinterpret_cast<const sockaddr_in*>(&addr_);
}

void InetAddress::setSockAddrInet(const sockaddr_in& addr) noexcept {
    std::memset(&addr_, 0, sizeof(addr_));
    *reinterpret_cast<sockaddr_in*>(&addr_) = addr;
    family_ = AF_INET;
}

// --- IPv6 accessors ---

const sockaddr_in6& InetAddress::getSockAddrInet6() const noexcept {
    return *reinterpret_cast<const sockaddr_in6*>(&addr_);
}

void InetAddress::setSockAddrInet6(const sockaddr_in6& addr) noexcept {
    std::memset(&addr_, 0, sizeof(addr_));
    *reinterpret_cast<sockaddr_in6*>(&addr_) = addr;
    family_ = AF_INET6;
}

// --- Generic accessors ---

const sockaddr* InetAddress::getSockAddr() const noexcept {
    return reinterpret_cast<const sockaddr*>(&addr_);
}

socklen_t InetAddress::getSockAddrLen() const noexcept {
    if (family_ == AF_INET6) {
        return static_cast<socklen_t>(sizeof(sockaddr_in6));
    }
    return static_cast<socklen_t>(sizeof(sockaddr_in));
}

// --- Family query ---

sa_family_t InetAddress::family() const noexcept {
    return family_;
}

bool InetAddress::isIpv4() const noexcept {
    return family_ == AF_INET;
}

bool InetAddress::isIpv6() const noexcept {
    return family_ == AF_INET6;
}

// --- Text conversion ---

std::string InetAddress::toIp() const {
    char buffer[INET6_ADDRSTRLEN] = {};
    if (family_ == AF_INET6) {
        const auto* addr6 = reinterpret_cast<const sockaddr_in6*>(&addr_);
        ::inet_ntop(AF_INET6, &addr6->sin6_addr, buffer, sizeof(buffer));
    } else {
        const auto* addr4 = reinterpret_cast<const sockaddr_in*>(&addr_);
        ::inet_ntop(AF_INET, &addr4->sin_addr, buffer, sizeof(buffer));
    }
    return buffer;
}

std::string InetAddress::toIpPort() const {
    if (family_ == AF_INET6) {
        return "[" + toIp() + "]:" + std::to_string(port());
    }
    return toIp() + ":" + std::to_string(port());
}

uint16_t InetAddress::port() const noexcept {
    if (family_ == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&addr_)->sin6_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in*>(&addr_)->sin_port);
}

}  // namespace mini::net
