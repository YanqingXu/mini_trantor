// Intent: dns_resolver / event_loop; callbacks may re-enter and loops may close.
#include "mini/coroutine/ResolveAwaitable.h"
#include "mini/coroutine/Task.h"
#include "mini/net/DnsResolver.h"
#include "mini/net/EventLoop.h"

#include <cassert>
#include <array>
#include <atomic>
#include <barrier>
#include <future>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using mini::net::DnsResolver;
using mini::net::EventLoop;

namespace {
void cacheCallbackMayClearAndResolveAgain() {
    DnsResolver resolver(1);
    resolver.enableCache();
    EventLoop loop;
    int callbacks = 0;
    resolver.resolve("localhost", 80, &loop, [&](auto initial) {
        assert(initial);
        ++callbacks;
        resolver.resolve("localhost", 81, &loop, [&](auto cached) {
            assert(cached && cached->front().port() == 81);
            ++callbacks;
            resolver.clearCache();
            resolver.enableCache();
            resolver.resolve("localhost", 82, &loop, [&](auto again) {
                assert(again && again->front().port() == 82);
                ++callbacks;
                loop.quit();
            });
        });
        assert(callbacks == 1); // even cache hits are queued, never inline
    });
    loop.loop();
    assert(callbacks == 3);
}

void invalidTargetsAreRejected() {
    DnsResolver resolver(1);
    EventLoop loop;
    bool nullLoop = false, emptyCallback = false;
    try { resolver.resolve("localhost", 80, nullptr, [](auto) {}); }
    catch (const std::invalid_argument&) { nullLoop = true; }
    try { resolver.resolve("localhost", 80, &loop, {}); }
    catch (const std::invalid_argument&) { emptyCallback = true; }
    assert(nullLoop && emptyCallback);
}

mini::coroutine::Task<void> resolving(std::shared_ptr<DnsResolver> resolver,
                                    EventLoop* loop, int* resumed, int* destroyed) {
    struct Probe { int* count; ~Probe() { ++*count; } } probe{destroyed};
    (void)co_await mini::coroutine::asyncResolve(resolver, loop, "localhost", 80);
    ++*resumed;
}

void destroyedResolveDoesNotResume() {
    auto resolver = std::make_shared<DnsResolver>(1);
    EventLoop loop;
    int resumed = 0, destroyed = 0;
    {
        auto task = resolving(resolver, &loop, &resumed, &destroyed);
        task.start();
    }
    assert(destroyed == 1);
    // One worker preserves queue order. This completion drains the earlier result.
    resolver->resolve("localhost", 81, &loop, [&](auto result) {
        assert(result);
        loop.quit();
    });
    loop.loop();
    assert(resumed == 0 && destroyed == 1);
}

void preCancelledCacheHitReportsCancellation() {
    DnsResolver resolver(1);
    resolver.enableCache();
    EventLoop loop;
    mini::coroutine::CancellationSource source;
    source.cancel();
    bool cancelled = false;
    resolver.resolve("localhost", 80, &loop, [&](auto initial) {
        assert(initial);
        resolver.resolve("localhost", 81, &loop, [&](auto result) {
            assert(!result && result.error() == mini::net::NetError::Cancelled);
            cancelled = true;
            loop.quit();
        }, source.token());
    });
    loop.loop();
    assert(cancelled);
}

void concurrentCancellationAndRegistration() {
    std::promise<EventLoop*> ready;
    auto readyFuture = ready.get_future();
    std::latch releaseLoop(1);
    std::thread worker([&] {
        EventLoop loop;
        ready.set_value(&loop);
        loop.loop();
        releaseLoop.wait();
    });
    auto* loop = readyFuture.get();
    constexpr int attempts = 200;
    std::array<mini::coroutine::CancellationSource, attempts> sources;
    std::array<std::atomic<int>, attempts> callbacks{};
    std::barrier race(3);
    std::latch finished(attempts);
    {
        DnsResolver resolver(2);
        resolver.enableCache();
        std::thread canceller([&] {
            for (auto& source : sources) {
                race.arrive_and_wait();
                source.cancel();
                race.arrive_and_wait();
            }
        });
        std::thread configure([&] {
            for (int i = 0; i < attempts; ++i) {
                race.arrive_and_wait();
                resolver.enableCache(std::chrono::seconds(1 + i));
                race.arrive_and_wait();
            }
        });
        for (int i = 0; i < attempts; ++i) {
            race.arrive_and_wait();
            resolver.resolve("localhost", 80, loop, [&, i](auto result) {
                assert(loop->isInLoopThread());
                assert(result || result.error() == mini::net::NetError::Cancelled);
                assert(callbacks[i].fetch_add(1) == 0);
                finished.count_down();
            }, sources[i].token());
            race.arrive_and_wait();
        }
        canceller.join();
        configure.join();
        finished.wait();
    } // workers stop posting before the loop lifetime barrier is released
    loop->quit();
    releaseLoop.count_down();
    worker.join();
    for (const auto& count : callbacks) { assert(count == 1); }
}

void loopMayCloseBeforeWorkersAndCancellationFinish() {
    auto resolver = std::make_shared<DnsResolver>(1);
    std::vector<std::weak_ptr<int>> callbackLifetimes;
    int resumed = 0, destroyed = 0;
    constexpr int batches = 40;
    for (int batch = 0; batch < batches; ++batch) {
        std::array<mini::coroutine::CancellationSource, 16> sources;
        {
            EventLoop loop;
            for (auto& source : sources) {
                auto lifetime = std::make_shared<int>(batch);
                callbackLifetimes.emplace_back(lifetime);
                resolver->resolve("localhost", 80, &loop,
                    [lifetime](auto) { assert(false); }, source.token());
            }
            // Frames are released on their owner before the loop goes away.
            auto task = resolving(resolver, &loop, &resumed, &destroyed);
            task.start();
        } // never-run loop discards queued callbacks; later posts must reject
        for (auto& source : sources) { source.cancel(); }
    }
    resolver.reset(); // joins all workers after every target loop was destroyed
    assert(resumed == 0 && destroyed == batches);
    for (auto& observed : callbackLifetimes) { assert(observed.expired()); }
}
} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "all";
    if (mode == "cache" || mode == "all") { cacheCallbackMayClearAndResolveAgain(); }
    if (mode == "frame" || mode == "all") { destroyedResolveDoesNotResume(); }
    if (mode == "cancel" || mode == "all") { preCancelledCacheHitReportsCancellation(); }
    if (mode == "race" || mode == "all") { concurrentCancellationAndRegistration(); }
    if (mode == "closed" || mode == "all") { loopMayCloseBeforeWorkersAndCancellationFinish(); }
    if (mode == "invalid" || mode == "all") { invalidTargetsAreRejected(); }
}
