#pragma once

// ResolveAwaitable 是基于 DnsResolver 的协程域名解析桥接。
// 它通过 DnsResolver::resolve 发起异步解析，完成后在 owner loop 线程恢复协程。
// 它不是独立调度器，不绕过 EventLoop 调度语义。

#include "mini/coroutine/CancellationToken.h"
#include "mini/coroutine/ResumeHandle.h"
#include "mini/net/DnsResolver.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/NetError.h"

#include <coroutine>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mini::coroutine {

/// Shared state between the resolve callback and the awaiting coroutine.
/// Ensures the coroutine handle is resumed exactly once.
struct ResolveState {
    enum class Phase { Unarmed, Pending, Completed, Abandoned };
    mini::net::EventLoop* loop{nullptr};
    detail::ResumeHandle handle{};
    mini::net::DnsResolver::ResolveResult result = std::unexpected(mini::net::NetError::ResolveFailed);
    Phase phase{Phase::Unarmed};
    CancellationSource lifetimeCancellation;
    std::optional<CancellationRegistration> tokenLink;
};

class ResolveAwaitable {
public:
    ResolveAwaitable(std::shared_ptr<mini::net::DnsResolver> resolver,
                     mini::net::EventLoop* loop,
                     std::string hostname, uint16_t port,
                     CancellationToken token = {})
        : resolver_(std::move(resolver)),
          state_(std::make_shared<ResolveState>()),
          hostname_(std::move(hostname)),
          port_(port),
          token_(std::move(token)) {
        if (!resolver_ || !loop) {
            throw std::invalid_argument("asyncResolve requires a resolver and EventLoop");
        }
        state_->loop = loop;
    }

    ResolveAwaitable(const ResolveAwaitable&) = delete;
    ResolveAwaitable& operator=(const ResolveAwaitable&) = delete;
    ResolveAwaitable(ResolveAwaitable&&) noexcept = default;
    ResolveAwaitable& operator=(ResolveAwaitable&&) = delete;
    ~ResolveAwaitable() { abandon(); }

    bool await_ready() const noexcept {
        return false;
    }

    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) {
        if (!state_ || state_->phase != ResolveState::Phase::Unarmed) {
            throw std::logic_error("ResolveAwaitable can only be awaited once");
        }
        auto state = state_;
        auto token = token_;
        if (!token) {
            if constexpr (requires(const Promise& promise) { promise.cancellationToken(); }) {
                token = handle.promise().cancellationToken();
            }
        }
        if (token) {
            state->tokenLink.emplace(token.registerCallback([source = state->lifetimeCancellation] {
                source.cancel();
            }));
        }
        state->handle = detail::borrowResume(handle);
        state->phase = ResolveState::Phase::Pending;
        auto operationToken = state->lifetimeCancellation.token();
        auto resolver = resolver_; // publication can complete on another loop
        resolver->resolve(hostname_, port_, state->loop,
            [state](mini::net::DnsResolver::ResolveResult addrs) mutable {
                // Delivered on owner loop thread by DnsResolver.
                if (state->phase == ResolveState::Phase::Pending) {
                    state->phase = ResolveState::Phase::Completed;
                    state->result = std::move(addrs);
                    state->tokenLink.reset();
                    auto resume = std::exchange(state->handle, {});
                    resume.resume();
                }
            },
            std::move(operationToken));
    }

    /// Returns the explicit result produced by DnsResolver.
    mini::net::Expected<std::vector<mini::net::InetAddress>> await_resume() {
        if (!state_ || state_->phase != ResolveState::Phase::Completed) {
            throw std::logic_error("resolve result requested before completion");
        }
        return std::move(state_->result);
    }

private:
    void abandon() noexcept {
        if (!state_ || state_->phase != ResolveState::Phase::Pending) { return; }
        state_->loop->assertInLoopThread();
        state_->phase = ResolveState::Phase::Abandoned;
        state_->handle = {};
        state_->tokenLink.reset();
        state_->lifetimeCancellation.cancel();
    }

    std::shared_ptr<mini::net::DnsResolver> resolver_;
    std::shared_ptr<ResolveState> state_;
    std::string hostname_;
    uint16_t port_;
    CancellationToken token_;
};

/// Factory function: creates a ResolveAwaitable for use with co_await.
///
/// Usage:
///   auto result = co_await mini::coroutine::asyncResolve(resolver, loop, "example.com", 80);
///   if (result) { /* use (*result)[0] */ }
inline ResolveAwaitable asyncResolve(std::shared_ptr<mini::net::DnsResolver> resolver,
                                     mini::net::EventLoop* loop,
                                     const std::string& hostname, uint16_t port,
                                     CancellationToken token = {}) {
    return ResolveAwaitable(std::move(resolver), loop, hostname, port, std::move(token));
}

}  // namespace mini::coroutine
