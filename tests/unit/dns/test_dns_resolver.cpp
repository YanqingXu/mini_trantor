// Unit tests for DnsResolver.
//
// Tests basic resolution, cache behavior, and error handling.
// localhost may produce IPv4 and IPv6 candidates in either order.

#include "mini/net/DnsResolver.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/NetError.h"
#include "mini/coroutine/CancellationToken.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

void assertLoopbackAddresses(const std::vector<mini::net::InetAddress>& addresses,
                             uint16_t port) {
    assert(!addresses.empty());
    for (const auto& address : addresses) {
        assert(address.isIpv4() || address.isIpv6());
        assert(address.toIp() == (address.isIpv4() ? "127.0.0.1" : "::1"));
        assert(address.port() == port);
    }
}

void assertCachedAddresses(const std::vector<mini::net::InetAddress>& original,
                          const std::vector<mini::net::InetAddress>& cached,
                          uint16_t port) {
    assert(cached.size() == original.size());
    for (std::size_t i = 0; i < original.size(); ++i) {
        assert(cached[i].family() == original[i].family());
        assert(cached[i].toIp() == original[i].toIp());
        assert(cached[i].port() == port);
        if (cached[i].isIpv6()) {
            assert(cached[i].getSockAddrInet6().sin6_scope_id ==
                   original[i].getSockAddrInet6().sin6_scope_id);
        }
    }
}

}  // namespace

int main() {
    // Unit 1: every localhost candidate is a loopback address with the requested port.
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::net::DnsResolver resolver(1);

        std::promise<mini::net::DnsResolver::ResolveResult> promise;
        auto future = promise.get_future();

        loop->runInLoop([&] {
            resolver.resolve("localhost", 8080, loop,
                [&](mini::net::DnsResolver::ResolveResult addrs) {
                    promise.set_value(std::move(addrs));
                });
        });

        auto addrs = future.get();
        assert(addrs);
        assertLoopbackAddresses(*addrs, 8080);

        loop->runInLoop([loop] { loop->quit(); });
        std::printf("  PASS: resolve localhost preserves valid loopback candidates\n");
    }

    // Unit 2: resolve invalid hostname returns explicit ResolveFailed
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::net::DnsResolver resolver(1);

        std::promise<mini::net::DnsResolver::ResolveResult> promise;
        auto future = promise.get_future();

        loop->runInLoop([&] {
            // Empty hostname is always rejected by getaddrinfo.
            resolver.resolve("", 80, loop,
                [&](mini::net::DnsResolver::ResolveResult addrs) {
                    promise.set_value(std::move(addrs));
                });
        });

        auto addrs = future.get();
        assert(!addrs);
        assert(addrs.error() == mini::net::NetError::ResolveFailed);

        loop->runInLoop([loop] { loop->quit(); });
        std::printf("  PASS: resolve invalid hostname returns explicit error\n");
    }

    // Unit 3: cache stores results — second resolve is served from cache
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::net::DnsResolver resolver(1);
        resolver.enableCache(60s);

        // First resolve: populates cache.
        std::promise<mini::net::DnsResolver::ResolveResult> p1;
        auto f1 = p1.get_future();
        loop->runInLoop([&] {
            resolver.resolve("localhost", 9090, loop,
                [&](mini::net::DnsResolver::ResolveResult addrs) {
                    p1.set_value(std::move(addrs));
                });
        });
        auto addrs1 = f1.get();
        assert(addrs1);
        assertLoopbackAddresses(*addrs1, 9090);

        // Second resolve: should hit cache (different port to verify port is applied).
        std::promise<mini::net::DnsResolver::ResolveResult> p2;
        auto f2 = p2.get_future();
        loop->runInLoop([&] {
            resolver.resolve("localhost", 7070, loop,
                [&](mini::net::DnsResolver::ResolveResult addrs) {
                    p2.set_value(std::move(addrs));
                });
        });
        auto addrs2 = f2.get();
        assert(addrs2);
        assertLoopbackAddresses(*addrs2, 7070);
        assertCachedAddresses(*addrs1, *addrs2, 7070);

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
                [&](mini::net::DnsResolver::ResolveResult result) {
                    assert(result);
                    p1.set_value();
                });
        });
        p1.get_future().get();

        // Clear cache.
        resolver.clearCache();

        // Resolve again after clear — must still work (goes to worker thread).
        std::promise<mini::net::DnsResolver::ResolveResult> p2;
        auto f2 = p2.get_future();
        loop->runInLoop([&] {
            resolver.resolve("localhost", 80, loop,
                [&](mini::net::DnsResolver::ResolveResult addrs) {
                    p2.set_value(std::move(addrs));
                });
        });
        auto addrs = f2.get();
        assert(addrs);
        assert(!addrs->empty());

        loop->runInLoop([loop] { loop->quit(); });
        std::printf("  PASS: clearCache works\n");
    }

    // Unit 5: literal and cached addresses cover both families independently of
    // the host's localhost configuration. Numeric IPv6 resolution needs no listener.
    for (const char* literal : {"127.0.0.1", "::1"}) {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::net::DnsResolver resolver(1);
        resolver.enableCache(60s);

        std::promise<mini::net::DnsResolver::ResolveResult> promise;
        auto future = promise.get_future();

        loop->runInLoop([&] {
            resolver.resolve(literal, 5000, loop,
                [&](mini::net::DnsResolver::ResolveResult addrs) {
                    promise.set_value(std::move(addrs));
                });
        });

        auto addrs = future.get();
        assert(addrs);
        assertLoopbackAddresses(*addrs, 5000);
        const auto family = std::string(literal) == "::1" ? AF_INET6 : AF_INET;
        for (const auto& address : *addrs) {
            assert(address.family() == family);
            assert(address.toIp() == literal);
        }

        std::promise<mini::net::DnsResolver::ResolveResult> cachedPromise;
        auto cachedFuture = cachedPromise.get_future();
        loop->runInLoop([&] {
            resolver.resolve(literal, 5001, loop,
                [&](mini::net::DnsResolver::ResolveResult result) {
                    cachedPromise.set_value(std::move(result));
                });
        });
        auto cached = cachedFuture.get();
        assert(cached);
        assertCachedAddresses(*addrs, *cached, 5001);

        loop->runInLoop([loop] { loop->quit(); });
        std::printf("  PASS: resolve and cache IP literal %s\n", literal);
    }

    // Unit 6: global shared instance
    {
        auto r1 = mini::net::DnsResolver::getShared();
        auto r2 = mini::net::DnsResolver::getShared();
        assert(r1 == r2);
        std::printf("  PASS: getShared returns same instance\n");
    }

    // Unit 7: already-cancelled token yields explicit Cancelled
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::net::DnsResolver resolver(1);
        mini::coroutine::CancellationSource source;
        source.cancel();

        std::promise<mini::net::DnsResolver::ResolveResult> promise;
        auto future = promise.get_future();

        loop->runInLoop([&] {
            resolver.resolve("localhost", 8080, loop,
                [&](mini::net::DnsResolver::ResolveResult addrs) {
                    promise.set_value(std::move(addrs));
                },
                source.token());
        });

        auto addrs = future.get();
        assert(!addrs);
        assert(addrs.error() == mini::net::NetError::Cancelled);

        loop->runInLoop([loop] { loop->quit(); });
        std::printf("  PASS: already-cancelled token yields explicit Cancelled\n");
    }

    std::printf("All DnsResolver unit tests passed.\n");
    return 0;
}
