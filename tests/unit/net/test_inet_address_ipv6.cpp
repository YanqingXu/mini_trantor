// InetAddress IPv6 unit tests.
//
// Verifies that InetAddress correctly handles IPv4/IPv6 dual-stack:
// - Construction from port, IP string, and raw sockaddr
// - Family detection (isIpv4 / isIpv6)
// - Text conversion (toIp / toIpPort / port)
// - Backward compatibility with existing IPv4 API

#include "mini/net/InetAddress.h"

#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>

int main() {
    // --- IPv4 backward compatibility ---

    // IPv4 any-address with port
    {
        mini::net::InetAddress addr(8080, false);
        assert(addr.isIpv4());
        assert(!addr.isIpv6());
        assert(addr.family() == AF_INET);
        assert(addr.port() == 8080);
        assert(addr.toIp() == "0.0.0.0");
        assert(addr.toIpPort() == "0.0.0.0:8080");
    }

    // IPv4 loopback with port
    {
        mini::net::InetAddress addr(9090, true);
        assert(addr.isIpv4());
        assert(addr.toIp() == "127.0.0.1");
        assert(addr.port() == 9090);
    }

    // IPv4 from IP string
    {
        mini::net::InetAddress addr("192.168.1.1", 443);
        assert(addr.isIpv4());
        assert(addr.toIp() == "192.168.1.1");
        assert(addr.toIpPort() == "192.168.1.1:443");
        assert(addr.port() == 443);
    }

    // IPv4 from sockaddr_in
    {
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(3000);
        ::inet_pton(AF_INET, "10.0.0.1", &sin.sin_addr);
        mini::net::InetAddress addr(sin);
        assert(addr.isIpv4());
        assert(addr.toIp() == "10.0.0.1");
        assert(addr.port() == 3000);
    }

    // IPv4 getSockAddrInet round-trip
    {
        mini::net::InetAddress addr("1.2.3.4", 5678);
        const sockaddr_in& sin = addr.getSockAddrInet();
        assert(sin.sin_family == AF_INET);
        assert(sin.sin_port == htons(5678));
        mini::net::InetAddress addr2(sin);
        assert(addr2.toIp() == "1.2.3.4");
        assert(addr2.port() == 5678);
    }

    // --- IPv6 ---

    // IPv6 from IP string
    {
        mini::net::InetAddress addr("::1", 8080);
        assert(addr.isIpv6());
        assert(!addr.isIpv4());
        assert(addr.family() == AF_INET6);
        assert(addr.toIp() == "::1");
        assert(addr.toIpPort() == "[::1]:8080");
        assert(addr.port() == 8080);
    }

    // IPv6 from bracketed IP string
    {
        mini::net::InetAddress addr("[::1]", 9090);
        assert(addr.isIpv6());
        assert(addr.toIp() == "::1");
        assert(addr.port() == 9090);
    }

    // IPv6 loopback
    {
        mini::net::InetAddress addr("::1", 443);
        assert(addr.isIpv6());
        assert(addr.toIp() == "::1");
        assert(addr.toIpPort() == "[::1]:443");
    }

    // IPv6 full address
    {
        mini::net::InetAddress addr("2001:db8::1", 80);
        assert(addr.isIpv6());
        assert(addr.toIp() == "2001:db8::1");
        assert(addr.port() == 80);
    }

    // IPv6 from sockaddr_in6
    {
        sockaddr_in6 sin6{};
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons(5000);
        ::inet_pton(AF_INET6, "fe80::1", &sin6.sin6_addr);
        mini::net::InetAddress addr(sin6);
        assert(addr.isIpv6());
        assert(addr.toIp() == "fe80::1");
        assert(addr.port() == 5000);
    }

    // IPv6 getSockAddrInet6 round-trip
    {
        mini::net::InetAddress addr("::1", 9999);
        const sockaddr_in6& sin6 = addr.getSockAddrInet6();
        assert(sin6.sin6_family == AF_INET6);
        assert(sin6.sin6_port == htons(9999));
        mini::net::InetAddress addr2(sin6);
        assert(addr2.toIp() == "::1");
        assert(addr2.port() == 9999);
    }

    // IPv6 from sockaddr_storage
    {
        mini::net::InetAddress addr("2001:db8::2", 3000);
        const sockaddr* sa = addr.getSockAddr();
        assert(sa->sa_family == AF_INET6);
        socklen_t len = addr.getSockAddrLen();
        assert(len == sizeof(sockaddr_in6));

        sockaddr_storage storage{};
        std::memcpy(&storage, sa, len);
        mini::net::InetAddress addr2(storage);
        assert(addr2.isIpv6());
        assert(addr2.toIp() == "2001:db8::2");
        assert(addr2.port() == 3000);
    }

    // IPv4 from sockaddr_storage
    {
        mini::net::InetAddress addr("10.0.0.1", 4000);
        const sockaddr* sa = addr.getSockAddr();
        assert(sa->sa_family == AF_INET);
        socklen_t len = addr.getSockAddrLen();
        assert(len == sizeof(sockaddr_in));

        sockaddr_storage storage{};
        std::memcpy(&storage, sa, len);
        mini::net::InetAddress addr2(storage);
        assert(addr2.isIpv4());
        assert(addr2.toIp() == "10.0.0.1");
        assert(addr2.port() == 4000);
    }

    // --- IPv6 setSockAddrInet6 ---

    {
        mini::net::InetAddress addr(0, false);  // IPv4 initially
        sockaddr_in6 sin6{};
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons(7000);
        ::inet_pton(AF_INET6, "::1", &sin6.sin6_addr);
        addr.setSockAddrInet6(sin6);
        assert(addr.isIpv6());
        assert(addr.toIp() == "::1");
        assert(addr.port() == 7000);
    }

    // --- Invalid addresses ---

    {
        bool threw = false;
        try {
            mini::net::InetAddress("not-an-ip", 80);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
    }

    {
        bool threw = false;
        try {
            mini::net::InetAddress("999.999.999.999", 80);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
    }

    return 0;
}
