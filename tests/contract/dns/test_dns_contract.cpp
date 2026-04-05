// DnsResolver contract tests.
//
// Core Module Change Gate:
// 1. Which loop/thread owns this module? — DnsResolver is standalone; worker pool
//    threads do resolution; callbacks delivered on the requesting EventLoop thread.
// 2. Who owns it and who releases it? — User or global shared instance.
//    Worker threads joined on destruction.
// 3. Which callbacks may re-enter? — Resolve callback may call resolve() again.
// 4. Cross-thread? — resolve() is thread-safe. Worker threads marshal results
//    to the requesting loop via runInLoop.
// 5. Test file? — This file.

#include "mini/net/DnsResolver.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main() {
    // Contract 1: callback is delivered on the specified EventLoop thread
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::net::DnsResolver resolver(1);

        std::promise<bool> promise;
        auto future = promise.get_future();

        loop->runInLoop([&] {
            resolver.resolve("localhost", 80, loop,
                [&](const std::vector<mini::net::InetAddress>&) {
                    promise.set_value(loop->isInLoopThread());
                });
        });

        assert(future.wait_for(5s) == std::future_status::ready);
        assert(future.get() == true);

        loop->runInLoop([loop] { loop->quit(); });
        std::printf("  PASS: callback delivered on requesting EventLoop thread\n");
    }

    // Contract 2: failed resolution delivers empty vector (not crash or hang)
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::net::DnsResolver resolver(1);

        std::promise<bool> promise;
        auto future = promise.get_future();

        loop->runInLoop([&] {
            // Empty hostname is always rejected by getaddrinfo.
            resolver.resolve("", 80, loop,
                [&](const std::vector<mini::net::InetAddress>& addrs) {
                    promise.set_value(addrs.empty());
                });
        });

        assert(future.wait_for(5s) == std::future_status::ready);
        assert(future.get() == true);

        loop->runInLoop([loop] { loop->quit(); });
        std::printf("  PASS: failed resolution delivers empty vector\n");
    }

    // Contract 3: multiple concurrent resolutions complete correctly
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::net::DnsResolver resolver(2);

        constexpr int kNumRequests = 5;
        std::promise<int> promise;
        auto future = promise.get_future();
        auto count = std::make_shared<int>(0);

        loop->runInLoop([&, count] {
            for (int i = 0; i < kNumRequests; ++i) {
                resolver.resolve("localhost", static_cast<uint16_t>(8000 + i), loop,
                    [&, count, i](const std::vector<mini::net::InetAddress>& addrs) {
                        assert(!addrs.empty());
                        assert(addrs[0].port() == static_cast<uint16_t>(8000 + i));
                        if (++(*count) == kNumRequests) {
                            promise.set_value(*count);
                        }
                    });
            }
        });

        assert(future.wait_for(10s) == std::future_status::ready);
        assert(future.get() == kNumRequests);

        loop->runInLoop([loop] { loop->quit(); });
        std::printf("  PASS: multiple concurrent resolutions complete\n");
    }

    // Contract 4: resolve from non-loop thread works (cross-thread resolve)
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::net::DnsResolver resolver(1);

        std::promise<bool> promise;
        auto future = promise.get_future();

        // Call resolve from main thread (not the loop thread).
        resolver.resolve("localhost", 80, loop,
            [&](const std::vector<mini::net::InetAddress>& addrs) {
                // Callback must still be on the loop thread.
                promise.set_value(loop->isInLoopThread() && !addrs.empty());
            });

        assert(future.wait_for(5s) == std::future_status::ready);
        assert(future.get() == true);

        loop->runInLoop([loop] { loop->quit(); });
        std::printf("  PASS: cross-thread resolve delivers on correct loop\n");
    }

    // Contract 5: cache hit delivers callback on the correct loop thread
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::net::DnsResolver resolver(1);
        resolver.enableCache(60s);

        // First resolve: populate cache.
        std::promise<void> p1;
        loop->runInLoop([&] {
            resolver.resolve("localhost", 80, loop,
                [&](const std::vector<mini::net::InetAddress>&) {
                    p1.set_value();
                });
        });
        p1.get_future().get();

        // Second resolve: cache hit — callback still on loop thread.
        std::promise<bool> p2;
        auto f2 = p2.get_future();
        loop->runInLoop([&] {
            resolver.resolve("localhost", 80, loop,
                [&](const std::vector<mini::net::InetAddress>& addrs) {
                    p2.set_value(loop->isInLoopThread() && !addrs.empty());
                });
        });

        assert(f2.wait_for(5s) == std::future_status::ready);
        assert(f2.get() == true);

        loop->runInLoop([loop] { loop->quit(); });
        std::printf("  PASS: cache hit callback on correct loop thread\n");
    }

    std::printf("All DnsResolver contract tests passed.\n");
    return 0;
}
