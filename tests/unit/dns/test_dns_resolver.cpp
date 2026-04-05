// Unit tests for DnsResolver.
//
// Tests basic resolution, cache behavior, and error handling.
// These tests use "localhost" which always resolves to 127.0.0.1 on Linux.

#include "mini/net/DnsResolver.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main() {
    // Unit 1: resolve "localhost" returns 127.0.0.1
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::net::DnsResolver resolver(1);

        std::promise<std::vector<mini::net::InetAddress>> promise;
        auto future = promise.get_future();

        loop->runInLoop([&] {
            resolver.resolve("localhost", 8080, loop,
                [&](const std::vector<mini::net::InetAddress>& addrs) {
                    promise.set_value(addrs);
                });
        });

        auto addrs = future.get();
        assert(!addrs.empty());
        assert(addrs[0].toIp() == "127.0.0.1");
        assert(addrs[0].port() == 8080);

        loop->runInLoop([loop] { loop->quit(); });
        std::printf("  PASS: resolve localhost returns 127.0.0.1\n");
    }

    // Unit 2: resolve invalid hostname returns empty vector
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::net::DnsResolver resolver(1);

        std::promise<std::vector<mini::net::InetAddress>> promise;
        auto future = promise.get_future();

        loop->runInLoop([&] {
            // Empty hostname is always rejected by getaddrinfo.
            resolver.resolve("", 80, loop,
                [&](const std::vector<mini::net::InetAddress>& addrs) {
                    promise.set_value(addrs);
                });
        });

        auto addrs = future.get();
        assert(addrs.empty());

        loop->runInLoop([loop] { loop->quit(); });
        std::printf("  PASS: resolve invalid hostname returns empty\n");
    }

    // Unit 3: cache stores results — second resolve is served from cache
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::net::DnsResolver resolver(1);
        resolver.enableCache(60s);

        // First resolve: populates cache.
        std::promise<std::vector<mini::net::InetAddress>> p1;
        auto f1 = p1.get_future();
        loop->runInLoop([&] {
            resolver.resolve("localhost", 9090, loop,
                [&](const std::vector<mini::net::InetAddress>& addrs) {
                    p1.set_value(addrs);
                });
        });
        auto addrs1 = f1.get();
        assert(!addrs1.empty());

        // Second resolve: should hit cache (different port to verify port is applied).
        std::promise<std::vector<mini::net::InetAddress>> p2;
        auto f2 = p2.get_future();
        loop->runInLoop([&] {
            resolver.resolve("localhost", 7070, loop,
                [&](const std::vector<mini::net::InetAddress>& addrs) {
                    p2.set_value(addrs);
                });
        });
        auto addrs2 = f2.get();
        assert(!addrs2.empty());
        assert(addrs2[0].toIp() == "127.0.0.1");
        assert(addrs2[0].port() == 7070);

        loop->runInLoop([loop] { loop->quit(); });
        std::printf("  PASS: cache stores and serves results\n");
    }

    // Unit 4: clearCache removes entries
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::net::DnsResolver resolver(1);
        resolver.enableCache(60s);

        // Populate cache.
        std::promise<void> p1;
        loop->runInLoop([&] {
            resolver.resolve("localhost", 80, loop,
                [&](const std::vector<mini::net::InetAddress>&) {
                    p1.set_value();
                });
        });
        p1.get_future().get();

        // Clear cache.
        resolver.clearCache();

        // Resolve again after clear — must still work (goes to worker thread).
        std::promise<std::vector<mini::net::InetAddress>> p2;
        auto f2 = p2.get_future();
        loop->runInLoop([&] {
            resolver.resolve("localhost", 80, loop,
                [&](const std::vector<mini::net::InetAddress>& addrs) {
                    p2.set_value(addrs);
                });
        });
        auto addrs = f2.get();
        assert(!addrs.empty());

        loop->runInLoop([loop] { loop->quit(); });
        std::printf("  PASS: clearCache works\n");
    }

    // Unit 5: resolve "127.0.0.1" literal address works
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::net::DnsResolver resolver(1);

        std::promise<std::vector<mini::net::InetAddress>> promise;
        auto future = promise.get_future();

        loop->runInLoop([&] {
            resolver.resolve("127.0.0.1", 5000, loop,
                [&](const std::vector<mini::net::InetAddress>& addrs) {
                    promise.set_value(addrs);
                });
        });

        auto addrs = future.get();
        assert(!addrs.empty());
        assert(addrs[0].toIp() == "127.0.0.1");
        assert(addrs[0].port() == 5000);

        loop->runInLoop([loop] { loop->quit(); });
        std::printf("  PASS: resolve IP literal works\n");
    }

    // Unit 6: global shared instance
    {
        auto r1 = mini::net::DnsResolver::getShared();
        auto r2 = mini::net::DnsResolver::getShared();
        assert(r1 == r2);
        std::printf("  PASS: getShared returns same instance\n");
    }

    std::printf("All DnsResolver unit tests passed.\n");
    return 0;
}
