#pragma once

// ResolveAwaitable 是基于 DnsResolver 的协程域名解析桥接。
// 它通过 DnsResolver::resolve 发起异步解析，完成后在 owner loop 线程恢复协程。
// 它不是独立调度器，不绕过 EventLoop 调度语义。

#include "mini/net/DnsResolver.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"

#include <coroutine>
#include <memory>
#include <string>
#include <vector>

namespace mini::coroutine {

/// Shared state between the resolve callback and the awaiting coroutine.
/// Ensures the coroutine handle is resumed exactly once.
struct ResolveState {
    mini::net::EventLoop* loop{nullptr};
    std::coroutine_handle<> handle{};
    std::vector<mini::net::InetAddress> result;
    bool resumed{false};
};

class ResolveAwaitable {
public:
    ResolveAwaitable(std::shared_ptr<mini::net::DnsResolver> resolver,
                     mini::net::EventLoop* loop,
                     std::string hostname, uint16_t port)
        : resolver_(std::move(resolver)),
          state_(std::make_shared<ResolveState>()),
          hostname_(std::move(hostname)),
          port_(port) {
        state_->loop = loop;
    }

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        state_->handle = handle;
        auto state = state_;
        resolver_->resolve(hostname_, port_, state_->loop,
            [state](const std::vector<mini::net::InetAddress>& addrs) {
                // Delivered on owner loop thread by DnsResolver.
                if (!state->resumed) {
                    state->resumed = true;
                    state->result = addrs;
                    state->handle.resume();
                }
            });
    }

    /// Returns the resolved addresses. Empty vector indicates failure.
    std::vector<mini::net::InetAddress> await_resume() {
        return std::move(state_->result);
    }

private:
    std::shared_ptr<mini::net::DnsResolver> resolver_;
    std::shared_ptr<ResolveState> state_;
    std::string hostname_;
    uint16_t port_;
};

/// Factory function: creates a ResolveAwaitable for use with co_await.
///
/// Usage:
///   auto addrs = co_await mini::coroutine::asyncResolve(resolver, loop, "example.com", 80);
///   if (!addrs.empty()) { /* use addrs[0] */ }
inline ResolveAwaitable asyncResolve(std::shared_ptr<mini::net::DnsResolver> resolver,
                                     mini::net::EventLoop* loop,
                                     const std::string& hostname, uint16_t port) {
    return ResolveAwaitable(std::move(resolver), loop, hostname, port);
}

}  // namespace mini::coroutine
