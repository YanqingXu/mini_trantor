#include "mini/net/InetAddress.h"

#include <arpa/inet.h>
#include <cstring>
#include <stdexcept>

namespace mini::net {

InetAddress::InetAddress(uint16_t port, bool loopbackOnly) {
    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    addr_.sin_port = htons(port);
}

InetAddress::InetAddress(std::string ip, uint16_t port) : InetAddress(port, false) {
    if (::inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) <= 0) {
        throw std::runtime_error("invalid IPv4 address: " + ip);
    }
}

InetAddress::InetAddress(const sockaddr_in& addr) : addr_(addr) {
}

const sockaddr_in& InetAddress::getSockAddrInet() const noexcept {
    return addr_;
}

void InetAddress::setSockAddrInet(const sockaddr_in& addr) noexcept {
    addr_ = addr;
}

std::string InetAddress::toIp() const {
    char buffer[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &addr_.sin_addr, buffer, sizeof(buffer));
    return buffer;
}

std::string InetAddress::toIpPort() const {
    return toIp() + ":" + std::to_string(port());
}

uint16_t InetAddress::port() const noexcept {
    return ntohs(addr_.sin_port);
}

}  // namespace mini::net
