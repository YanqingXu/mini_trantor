#include "mini/net/udp/PathMtuSignalAdapter.h"

#include <cassert>
#include <optional>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

void testUdpPayloadSizeFromPathMtu() {
    using mini::net::udp::PathMtuSignalAdapter;

    assert(PathMtuSignalAdapter::udpPayloadSizeFromPathMtu(1500, AF_INET) == 1472);
    assert(PathMtuSignalAdapter::udpPayloadSizeFromPathMtu(28, AF_INET) == 0);
    assert(PathMtuSignalAdapter::udpPayloadSizeFromPathMtu(1280, AF_INET6) == 1232);
    assert(PathMtuSignalAdapter::udpPayloadSizeFromPathMtu(48, AF_INET6) == 0);
    assert(PathMtuSignalAdapter::udpPayloadSizeFromPathMtu(0, AF_INET) == 0);
}

void testPlatformCapabilities() {
    using mini::net::udp::PathMtuSignalAdapter;

    const auto ipv4 = PathMtuSignalAdapter::platformCapabilities(AF_INET);
    assert(ipv4.supportsIpv4);
    assert(!ipv4.supportsIpv6);

    const auto ipv6 = PathMtuSignalAdapter::platformCapabilities(AF_INET6);
    assert(!ipv6.supportsIpv4);
    assert(ipv6.supportsIpv6);

#ifdef __linux__
    assert(ipv4.source == PathMtuSignalAdapter::PlatformSource::kLinuxErrorQueue);
    assert(ipv4.canConfigureAsyncSignals);
    assert(ipv4.canDrainAsyncSignals);
    assert(ipv6.source == PathMtuSignalAdapter::PlatformSource::kLinuxErrorQueue);
    assert(ipv6.canConfigureAsyncSignals);
    assert(ipv6.canDrainAsyncSignals);
#else
    (void)ipv4;
    (void)ipv6;
#endif
}

void testConfigureUdpErrorQueueIpv4() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    assert(fd >= 0);

#ifdef __linux__
    assert(mini::net::udp::PathMtuSignalAdapter::configurePlatformPathMtuSignals(
        fd,
        AF_INET,
        true));
    assert(mini::net::udp::PathMtuSignalAdapter::configurePlatformPathMtuSignals(
        fd,
        AF_INET,
        false));
    assert(mini::net::udp::PathMtuSignalAdapter::configureUdpErrorQueue(
        fd,
        AF_INET,
        true));
    assert(mini::net::udp::PathMtuSignalAdapter::configureUdpErrorQueue(
        fd,
        AF_INET,
        false));
#else
    assert(!mini::net::udp::PathMtuSignalAdapter::configurePlatformPathMtuSignals(
        fd,
        AF_INET,
        true));
    assert(!mini::net::udp::PathMtuSignalAdapter::configureUdpErrorQueue(
        fd,
        AF_INET,
        true));
#endif

    ::close(fd);
}

void testConfigureUdpErrorQueueIpv6() {
    const int fd = ::socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    assert(fd >= 0);

#ifdef __linux__
    assert(mini::net::udp::PathMtuSignalAdapter::configurePlatformPathMtuSignals(
        fd,
        AF_INET6,
        true));
    assert(mini::net::udp::PathMtuSignalAdapter::configurePlatformPathMtuSignals(
        fd,
        AF_INET6,
        false));
    assert(mini::net::udp::PathMtuSignalAdapter::configureUdpErrorQueue(
        fd,
        AF_INET6,
        true));
    assert(mini::net::udp::PathMtuSignalAdapter::configureUdpErrorQueue(
        fd,
        AF_INET6,
        false));
#else
    assert(!mini::net::udp::PathMtuSignalAdapter::configurePlatformPathMtuSignals(
        fd,
        AF_INET6,
        true));
    assert(!mini::net::udp::PathMtuSignalAdapter::configureUdpErrorQueue(
        fd,
        AF_INET6,
        true));
#endif

    ::close(fd);
}

void testQueryConnectedUdpPayloadMtuRequiresConnectedPath() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    assert(fd >= 0);

    const auto mtu =
        mini::net::udp::PathMtuSignalAdapter::queryConnectedUdpPayloadMtu(fd, AF_INET);
    assert(!mtu.has_value());

    ::close(fd);
}

}  // namespace

int main() {
    testUdpPayloadSizeFromPathMtu();
    testPlatformCapabilities();
    testConfigureUdpErrorQueueIpv4();
    testConfigureUdpErrorQueueIpv6();
    testQueryConnectedUdpPayloadMtuRequiresConnectedPath();
    return 0;
}
