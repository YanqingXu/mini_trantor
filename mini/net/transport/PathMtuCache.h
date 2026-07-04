#pragma once

// PathMtuCache — thread-safe, value-semantic path MTU facts shared by transports.
//
// It stores only peer-keyed datagram payload sizing hints and cooldown state.
// It does not own sockets, EventLoops, sessions, or upper-layer game objects.

#include "mini/base/noncopyable.h"
#include "mini/net/InetAddress.h"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mini::net::transport {

struct PathMtuCacheEntry {
    std::size_t confirmedDatagramPayloadSize{0};
    std::chrono::steady_clock::time_point cooldownUntil{};
    std::size_t blackholeCount{0};
};

class PathMtuCache final : private mini::base::noncopyable {
public:
    using Clock = std::chrono::steady_clock;

    static std::string keyForPeer(const InetAddress& peerAddress);

    std::optional<PathMtuCacheEntry> find(const InetAddress& peerAddress) const;
    std::optional<PathMtuCacheEntry> find(std::string_view pathKey) const;

    void recordSuccess(const InetAddress& peerAddress, std::size_t confirmedDatagramPayloadSize);
    void recordSuccess(std::string_view pathKey, std::size_t confirmedDatagramPayloadSize);

    void recordBlackhole(const InetAddress& peerAddress,
                         std::size_t confirmedDatagramPayloadSize,
                         Clock::time_point cooldownUntil,
                         std::size_t blackholeCount);
    void recordBlackhole(std::string_view pathKey,
                         std::size_t confirmedDatagramPayloadSize,
                         Clock::time_point cooldownUntil,
                         std::size_t blackholeCount);

    void recordFailure(const InetAddress& peerAddress,
                       std::size_t safeDatagramPayloadSize,
                       Clock::time_point cooldownUntil,
                       std::size_t blackholeCount);
    void recordFailure(std::string_view pathKey,
                       std::size_t safeDatagramPayloadSize,
                       Clock::time_point cooldownUntil,
                       std::size_t blackholeCount);

    void clearCooldown(const InetAddress& peerAddress);
    void clearCooldown(std::string_view pathKey);
    void erase(const InetAddress& peerAddress);
    void erase(std::string_view pathKey);
    void clear();
    std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, PathMtuCacheEntry> entries_;
};

}  // namespace mini::net::transport
