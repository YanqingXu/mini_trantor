#include "mini/net/InetAddress.h"
#include "mini/net/transport/PathMtuCache.h"

#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

namespace {

using Cache = mini::net::transport::PathMtuCache;
using namespace std::chrono_literals;

void testRecordSuccessKeepsLargestConfirmedSize() {
    Cache cache;
    const mini::net::InetAddress peer("127.0.0.1", 7001);

    assert(!cache.find(peer).has_value());
    cache.recordSuccess(peer, 1200);
    cache.recordSuccess(peer, 1000);

    auto entry = cache.find(peer);
    assert(entry.has_value());
    assert(entry->confirmedDatagramPayloadSize == 1200);
    assert(entry->cooldownUntil == Cache::Clock::time_point{});
    assert(entry->blackholeCount == 0);

    cache.recordSuccess(peer, 1400);
    entry = cache.find(peer);
    assert(entry.has_value());
    assert(entry->confirmedDatagramPayloadSize == 1400);
    assert(cache.size() == 1);
}

void testFailureDowngradesSizeAndCooldownCanBeCleared() {
    Cache cache;
    const mini::net::InetAddress peer("127.0.0.1", 7002);
    const auto cooldownUntil = Cache::Clock::now() + 2s;

    cache.recordSuccess(peer, 1400);
    cache.recordFailure(peer, 900, cooldownUntil, 2);

    auto entry = cache.find(peer);
    assert(entry.has_value());
    assert(entry->confirmedDatagramPayloadSize == 900);
    assert(entry->cooldownUntil == cooldownUntil);
    assert(entry->blackholeCount == 2);

    cache.clearCooldown(peer);
    entry = cache.find(peer);
    assert(entry.has_value());
    assert(entry->confirmedDatagramPayloadSize == 900);
    assert(entry->cooldownUntil == Cache::Clock::time_point{});
    assert(entry->blackholeCount == 0);
}

void testBlackholePreservesLargestConfirmedSizeAndLongestCooldown() {
    Cache cache;
    const mini::net::InetAddress peer("127.0.0.1", 7003);
    const auto earlyCooldown = Cache::Clock::now() + 1s;
    const auto laterCooldown = Cache::Clock::now() + 3s;

    cache.recordBlackhole(peer, 1200, earlyCooldown, 1);
    cache.recordBlackhole(peer, 1000, laterCooldown, 3);

    auto entry = cache.find(peer);
    assert(entry.has_value());
    assert(entry->confirmedDatagramPayloadSize == 1200);
    assert(entry->cooldownUntil == laterCooldown);
    assert(entry->blackholeCount == 3);
}

void testConcurrentSharedAccessKeepsSinglePeerEntry() {
    Cache cache;
    const mini::net::InetAddress peer("127.0.0.1", 7004);
    std::vector<std::thread> workers;

    for (std::size_t worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&cache, peer, worker] {
            for (std::size_t i = 0; i < 128; ++i) {
                cache.recordSuccess(peer, 900 + worker * 100 + i);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    const auto entry = cache.find(peer);
    assert(entry.has_value());
    assert(entry->confirmedDatagramPayloadSize == 1327);
    assert(cache.size() == 1);
}

void testEraseAndClear() {
    Cache cache;
    const mini::net::InetAddress first("127.0.0.1", 7005);
    const mini::net::InetAddress second("127.0.0.1", 7006);

    cache.recordSuccess(first, 1000);
    cache.recordSuccess(second, 1100);
    assert(cache.size() == 2);

    cache.erase(first);
    assert(!cache.find(first).has_value());
    assert(cache.find(second).has_value());
    assert(cache.size() == 1);

    cache.clear();
    assert(cache.size() == 0);
}

}  // namespace

int main() {
    testRecordSuccessKeepsLargestConfirmedSize();
    testFailureDowngradesSizeAndCooldownCanBeCleared();
    testBlackholePreservesLargestConfirmedSizeAndLongestCooldown();
    testConcurrentSharedAccessKeepsSinglePeerEntry();
    testEraseAndClear();
    return 0;
}
