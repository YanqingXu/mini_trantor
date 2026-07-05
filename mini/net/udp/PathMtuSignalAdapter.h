#pragma once

// PathMtuSignalAdapter isolates platform-specific UDP PMTU signal plumbing from
// UdpSocket. Linux uses MSG_ERRQUEUE; other platforms expose capability facts
// and guarded hooks so KCP policy can stay independent from OS-specific APIs.

#include "mini/net/udp/PathMtuSignal.h"
#include "mini/net/SocketTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace mini::net::udp {

class PathMtuSignalAdapter {
public:
    using FailureCallback = std::function<void(const PathMtuFailure& failure)>;
    using ErrorCallback = std::function<void(int errorCode)>;

    enum class PlatformSource {
        kUnsupported = 0,
        kLinuxErrorQueue,
        kBsdRecvPathMtu,
        kConnectedSocketMtuQuery,
    };

    struct PlatformCapabilities {
        PlatformSource source{PlatformSource::kUnsupported};
        bool canConfigureAsyncSignals{false};
        bool canDrainAsyncSignals{false};
        bool canQueryConnectedPathMtu{false};
        bool requiresConnectedSocketForQuery{false};
        bool supportsIpv4{false};
        bool supportsIpv6{false};
    };

    static PlatformCapabilities platformCapabilities(sa_family_t family) noexcept;
    static bool configurePlatformPathMtuSignals(SocketFd fd, sa_family_t family, bool enabled);
    static bool drainPlatformPathMtuSignals(SocketFd fd,
                                            const FailureCallback& failureCallback,
                                            const ErrorCallback& errorCallback);
    static std::optional<std::size_t> queryConnectedUdpPayloadMtu(SocketFd fd,
                                                                  sa_family_t family) noexcept;

    static bool configureUdpErrorQueue(SocketFd fd, sa_family_t family, bool enabled);
    static bool drainUdpErrorQueue(SocketFd fd,
                                   const FailureCallback& failureCallback,
                                   const ErrorCallback& errorCallback);
    static std::size_t udpPayloadSizeFromPathMtu(std::uint32_t pathMtu,
                                                 sa_family_t family) noexcept;
};

}  // namespace mini::net::udp
