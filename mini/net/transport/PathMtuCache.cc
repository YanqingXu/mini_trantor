#include "mini/net/transport/PathMtuCache.h"

#include <algorithm>

namespace mini::net::transport {

std::string PathMtuCache::keyForPeer(const InetAddress& peerAddress) {
    return peerAddress.toIpPort();
}

std::optional<PathMtuCacheEntry> PathMtuCache::find(const InetAddress& peerAddress) const {
    return find(keyForPeer(peerAddress));
}

std::optional<PathMtuCacheEntry> PathMtuCache::find(std::string_view pathKey) const {
    std::scoped_lock lock(mutex_);
    const auto it = entries_.find(std::string(pathKey));
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void PathMtuCache::recordSuccess(const InetAddress& peerAddress,
                                 std::size_t confirmedDatagramPayloadSize) {
    recordSuccess(keyForPeer(peerAddress), confirmedDatagramPayloadSize);
}

void PathMtuCache::recordSuccess(std::string_view pathKey,
                                 std::size_t confirmedDatagramPayloadSize) {
    if (confirmedDatagramPayloadSize == 0) {
        return;
    }

    std::scoped_lock lock(mutex_);
    auto& entry = entries_[std::string(pathKey)];
    entry.confirmedDatagramPayloadSize =
        std::max(entry.confirmedDatagramPayloadSize, confirmedDatagramPayloadSize);
    entry.cooldownUntil = {};
    entry.blackholeCount = 0;
}

void PathMtuCache::recordBlackhole(const InetAddress& peerAddress,
                                   std::size_t confirmedDatagramPayloadSize,
                                   Clock::time_point cooldownUntil,
                                   std::size_t blackholeCount) {
    recordBlackhole(keyForPeer(peerAddress),
                    confirmedDatagramPayloadSize,
                    cooldownUntil,
                    blackholeCount);
}

void PathMtuCache::recordBlackhole(std::string_view pathKey,
                                   std::size_t confirmedDatagramPayloadSize,
                                   Clock::time_point cooldownUntil,
                                   std::size_t blackholeCount) {
    std::scoped_lock lock(mutex_);
    auto& entry = entries_[std::string(pathKey)];
    entry.confirmedDatagramPayloadSize =
        std::max(entry.confirmedDatagramPayloadSize, confirmedDatagramPayloadSize);
    if (cooldownUntil != Clock::time_point{} &&
        (entry.cooldownUntil == Clock::time_point{} || entry.cooldownUntil < cooldownUntil)) {
        entry.cooldownUntil = cooldownUntil;
    }
    entry.blackholeCount = std::max(entry.blackholeCount, blackholeCount);
}

void PathMtuCache::recordFailure(const InetAddress& peerAddress,
                                 std::size_t safeDatagramPayloadSize,
                                 Clock::time_point cooldownUntil,
                                 std::size_t blackholeCount) {
    recordFailure(keyForPeer(peerAddress),
                  safeDatagramPayloadSize,
                  cooldownUntil,
                  blackholeCount);
}

void PathMtuCache::recordFailure(std::string_view pathKey,
                                 std::size_t safeDatagramPayloadSize,
                                 Clock::time_point cooldownUntil,
                                 std::size_t blackholeCount) {
    if (safeDatagramPayloadSize == 0) {
        return;
    }

    std::scoped_lock lock(mutex_);
    auto& entry = entries_[std::string(pathKey)];
    entry.confirmedDatagramPayloadSize = safeDatagramPayloadSize;
    if (cooldownUntil != Clock::time_point{} &&
        (entry.cooldownUntil == Clock::time_point{} || entry.cooldownUntil < cooldownUntil)) {
        entry.cooldownUntil = cooldownUntil;
    }
    entry.blackholeCount = std::max(entry.blackholeCount, blackholeCount);
}

void PathMtuCache::clearCooldown(const InetAddress& peerAddress) {
    clearCooldown(keyForPeer(peerAddress));
}

void PathMtuCache::clearCooldown(std::string_view pathKey) {
    std::scoped_lock lock(mutex_);
    const auto it = entries_.find(std::string(pathKey));
    if (it == entries_.end()) {
        return;
    }
    it->second.cooldownUntil = {};
    it->second.blackholeCount = 0;
}

void PathMtuCache::erase(const InetAddress& peerAddress) {
    erase(keyForPeer(peerAddress));
}

void PathMtuCache::erase(std::string_view pathKey) {
    std::scoped_lock lock(mutex_);
    entries_.erase(std::string(pathKey));
}

void PathMtuCache::clear() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
}

std::size_t PathMtuCache::size() const {
    std::scoped_lock lock(mutex_);
    return entries_.size();
}

}  // namespace mini::net::transport
