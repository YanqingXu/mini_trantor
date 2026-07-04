#pragma once

// Shared value type for UDP path MTU failure samples. Producers may be Linux
// error queue, local send EMSGSIZE, or raw ICMP listeners; consumers own policy.

#include "mini/net/InetAddress.h"

#include <cstddef>
#include <string>

namespace mini::net::udp {

inline constexpr std::size_t kMaxPathMtuQuotedUdpPayloadPrefix = 64;

enum class PathMtuSignalSource {
    kUnknown = 0,
    kLocalSend,
    kPlatformErrorQueue,
    kRawIcmp,
};

struct PathMtuFailure {
    InetAddress peerAddr;
    std::size_t failedDatagramPayloadSize{0};
    std::size_t suggestedDatagramPayloadSize{0};
    int errorCode{0};
    PathMtuSignalSource source{PathMtuSignalSource::kUnknown};
    std::string quotedUdpPayloadPrefix;
};

}  // namespace mini::net::udp
