#include "mini/net/udp/PathMtuSignalAdapter.h"

#include "mini/base/Logger.h"
#include "mini/net/InetAddress.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>

#ifdef __linux__
#include <linux/errqueue.h>
#endif

namespace mini::net::udp {

namespace {

#if defined(IP_RECVPATHMTU)
constexpr bool kHasIpv4BsdRecvPathMtu = true;
#else
constexpr bool kHasIpv4BsdRecvPathMtu = false;
#endif

#if defined(IPV6_RECVPATHMTU)
constexpr bool kHasIpv6BsdRecvPathMtu = true;
#else
constexpr bool kHasIpv6BsdRecvPathMtu = false;
#endif

#ifdef __linux__
constexpr std::size_t kUdpPacketBufferSize = 65535;

bool toInetAddress(const sockaddr* addr, socklen_t len, InetAddress& out) {
    if (addr == nullptr) {
        return false;
    }
    if (addr->sa_family == AF_INET && len >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
        const auto* addr4 = reinterpret_cast<const sockaddr_in*>(addr);
        if (addr4->sin_port == 0) {
            return false;
        }
        out = InetAddress(*addr4);
        return true;
    }
    if (addr->sa_family == AF_INET6 && len >= static_cast<socklen_t>(sizeof(sockaddr_in6))) {
        const auto* addr6 = reinterpret_cast<const sockaddr_in6*>(addr);
        if (addr6->sin6_port == 0) {
            return false;
        }
        out = InetAddress(*addr6);
        return true;
    }
    return false;
}

std::string quotedUdpPayloadPrefix(const std::array<char, kUdpPacketBufferSize>& packet,
                                   ssize_t length) {
    if (length <= 0) {
        return {};
    }
    const auto prefixLength = std::min(
        static_cast<std::size_t>(length),
        kMaxPathMtuQuotedUdpPayloadPrefix);
    return std::string(packet.data(), prefixLength);
}
#endif

}  // namespace

PathMtuSignalAdapter::PlatformCapabilities
PathMtuSignalAdapter::platformCapabilities(sa_family_t family) noexcept {
    PlatformCapabilities capabilities;
    capabilities.supportsIpv4 = family == AF_INET;
    capabilities.supportsIpv6 = family == AF_INET6;

#ifdef __linux__
    if (family == AF_INET || family == AF_INET6) {
        capabilities.source = PlatformSource::kLinuxErrorQueue;
        capabilities.canConfigureAsyncSignals = true;
        capabilities.canDrainAsyncSignals = true;
#if defined(IP_MTU) || defined(IPV6_MTU)
        capabilities.canQueryConnectedPathMtu = true;
        capabilities.requiresConnectedSocketForQuery = true;
#endif
    }
    return capabilities;
#else
#if defined(IP_RECVPATHMTU) || defined(IPV6_RECVPATHMTU)
    if ((family == AF_INET && kHasIpv4BsdRecvPathMtu) ||
        (family == AF_INET6 && kHasIpv6BsdRecvPathMtu)) {
        capabilities.source = PlatformSource::kBsdRecvPathMtu;
        capabilities.canConfigureAsyncSignals = true;
        capabilities.canDrainAsyncSignals = false;
    }
#endif
#if defined(IP_MTU) || defined(IPV6_MTU)
    if (family == AF_INET || family == AF_INET6) {
        if (capabilities.source == PlatformSource::kUnsupported) {
            capabilities.source = PlatformSource::kConnectedSocketMtuQuery;
        }
        capabilities.canQueryConnectedPathMtu = true;
        capabilities.requiresConnectedSocketForQuery = true;
    }
#endif
    return capabilities;
#endif
}

bool PathMtuSignalAdapter::configurePlatformPathMtuSignals(int fd,
                                                           sa_family_t family,
                                                           bool enabled) {
    return configureUdpErrorQueue(fd, family, enabled);
}

bool PathMtuSignalAdapter::drainPlatformPathMtuSignals(
    int fd,
    const FailureCallback& failureCallback,
    const ErrorCallback& errorCallback) {
    return drainUdpErrorQueue(fd, failureCallback, errorCallback);
}

std::optional<std::size_t> PathMtuSignalAdapter::queryConnectedUdpPayloadMtu(
    int fd,
    sa_family_t family) noexcept {
    std::uint32_t pathMtu = 0;
    socklen_t len = static_cast<socklen_t>(sizeof(pathMtu));

    if (family == AF_INET) {
#if defined(IP_MTU)
        if (::getsockopt(fd, IPPROTO_IP, IP_MTU, &pathMtu, &len) == 0) {
            return udpPayloadSizeFromPathMtu(pathMtu, family);
        }
#endif
        return std::nullopt;
    }

    if (family == AF_INET6) {
#if defined(IPV6_MTU)
        if (::getsockopt(fd, IPPROTO_IPV6, IPV6_MTU, &pathMtu, &len) == 0) {
            return udpPayloadSizeFromPathMtu(pathMtu, family);
        }
#endif
        return std::nullopt;
    }

    return std::nullopt;
}

bool PathMtuSignalAdapter::configureUdpErrorQueue(int fd,
                                                  sa_family_t family,
                                                  bool enabled) {
#ifdef __linux__
    const int value = enabled ? 1 : 0;
    bool attempted = false;
    bool succeeded = false;

    const auto trySet = [&](int level, int option) {
        attempted = true;
        if (::setsockopt(fd, level, option, &value, sizeof(value)) == 0) {
            succeeded = true;
            return;
        }
        if (errno != ENOPROTOOPT && errno != EINVAL) {
            LOG_SYSERR << "PathMtuSignalAdapter::configureUdpErrorQueue setsockopt failed: "
                       << std::strerror(errno);
        }
    };

    if (family == AF_INET) {
        trySet(IPPROTO_IP, IP_RECVERR);
    } else if (family == AF_INET6) {
        trySet(IPPROTO_IPV6, IPV6_RECVERR);
        trySet(IPPROTO_IP, IP_RECVERR);
    }

    if (!enabled) {
        return attempted;
    }
    return attempted && succeeded;
#else
    (void)fd;
    (void)family;
    (void)enabled;
    return false;
#endif
}

bool PathMtuSignalAdapter::drainUdpErrorQueue(int fd,
                                              const FailureCallback& failureCallback,
                                              const ErrorCallback& errorCallback) {
#ifdef __linux__
    bool handled = false;
    while (true) {
        std::array<char, kUdpPacketBufferSize> packetBuffer{};
        std::array<char, 512> controlBuffer{};
        sockaddr_storage messageAddress{};
        iovec iov{};
        iov.iov_base = packetBuffer.data();
        iov.iov_len = packetBuffer.size();

        msghdr message{};
        message.msg_name = &messageAddress;
        message.msg_namelen = static_cast<socklen_t>(sizeof(messageAddress));
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        message.msg_control = controlBuffer.data();
        message.msg_controllen = controlBuffer.size();

        const ssize_t n = ::recvmsg(fd, &message, MSG_ERRQUEUE | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return handled;
            }
            if (errorCallback) {
                errorCallback(errno);
            }
            return handled;
        }

        for (cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
             cmsg != nullptr;
             cmsg = CMSG_NXTHDR(&message, cmsg)) {
            const bool isIpv4Error =
                cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_RECVERR;
            const bool isIpv6Error =
                cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_RECVERR;
            if (!isIpv4Error && !isIpv6Error) {
                continue;
            }
            if (cmsg->cmsg_len < CMSG_LEN(sizeof(sock_extended_err))) {
                continue;
            }

            const auto* extendedError =
                reinterpret_cast<const sock_extended_err*>(CMSG_DATA(cmsg));
            if (extendedError->ee_errno != EMSGSIZE) {
                continue;
            }
            if (extendedError->ee_origin != SO_EE_ORIGIN_LOCAL &&
                extendedError->ee_origin != SO_EE_ORIGIN_ICMP &&
                extendedError->ee_origin != SO_EE_ORIGIN_ICMP6) {
                continue;
            }

            InetAddress peerAddr;
            bool hasPeer = toInetAddress(
                reinterpret_cast<const sockaddr*>(&messageAddress),
                message.msg_namelen,
                peerAddr);
            if (!hasPeer) {
                const auto* offender = SO_EE_OFFENDER(extendedError);
                if (offender != nullptr) {
                    hasPeer = toInetAddress(
                        offender,
                        offender->sa_family == AF_INET6
                            ? static_cast<socklen_t>(sizeof(sockaddr_in6))
                            : static_cast<socklen_t>(sizeof(sockaddr_in)),
                        peerAddr);
                }
            }
            if (!hasPeer) {
                continue;
            }

            if (failureCallback) {
                PathMtuFailure failure;
                failure.peerAddr = peerAddr;
                failure.failedDatagramPayloadSize =
                    n > 0 ? static_cast<std::size_t>(n) : 0;
                failure.suggestedDatagramPayloadSize =
                    udpPayloadSizeFromPathMtu(extendedError->ee_info, peerAddr.family());
                failure.errorCode = static_cast<int>(extendedError->ee_errno);
                failure.source = PathMtuSignalSource::kPlatformErrorQueue;
                failure.quotedUdpPayloadPrefix = quotedUdpPayloadPrefix(packetBuffer, n);
                failureCallback(failure);
            }
            handled = true;
        }
    }
#else
    (void)fd;
    (void)failureCallback;
    (void)errorCallback;
    return false;
#endif
}

std::size_t PathMtuSignalAdapter::udpPayloadSizeFromPathMtu(std::uint32_t pathMtu,
                                                            sa_family_t family) noexcept {
    if (pathMtu == 0) {
        return 0;
    }
    const std::size_t overhead = family == AF_INET6 ? 48 : 28;
    if (pathMtu <= overhead) {
        return 0;
    }
    return static_cast<std::size_t>(pathMtu) - overhead;
}

}  // namespace mini::net::udp
