#pragma once

// SleepAwaitable 在 owner EventLoop 上注册、完成或注销一次等待。
// 取消返回 Cancelled；析构只注销，绝不恢复已被销毁的 coroutine frame。

#include "mini/coroutine/CancellationToken.h"
#include "mini/net/EventLoop.h"
#include "mini/net/NetError.h"
#include "mini/net/TimerId.h"

#include <chrono>
#include <coroutine>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace mini::coroutine {

/// Diagnostic state: inspect only on the owner loop or after a completion barrier.
struct SleepState {
    enum class Phase { Unarmed, Pending, Expired, Cancelled, Abandoned };
    mini::net::EventLoop* loop{nullptr};
    std::coroutine_handle<> handle{};
    mini::net::TimerId timerId{};
    Phase phase{Phase::Unarmed};
    std::optional<CancellationRegistration> registration;
};

class SleepAwaitable {
public:
    using Duration = std::chrono::steady_clock::duration;

    SleepAwaitable(mini::net::EventLoop* loop, Duration duration, CancellationToken token = {})
        : state_(std::make_shared<SleepState>()), duration_(duration), token_(std::move(token)) {
        if (!loop) {
            throw std::invalid_argument("asyncSleep requires an EventLoop");
        }
        state_->loop = loop;
    }

    SleepAwaitable(const SleepAwaitable&) = delete;
    SleepAwaitable& operator=(const SleepAwaitable&) = delete;
    SleepAwaitable(SleepAwaitable&&) noexcept = default;
    SleepAwaitable& operator=(SleepAwaitable&& other) noexcept {
        if (this != &other) {
            abandon();
            state_ = std::move(other.state_);
            duration_ = other.duration_;
            token_ = std::move(other.token_);
        }
        return *this;
    }

    ~SleepAwaitable() { abandon(); }

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) {
        if (!state_ || state_->phase != SleepState::Phase::Unarmed) {
            throw std::logic_error("SleepAwaitable can only be awaited once");
        }
        auto state = state_;
        auto token = token_;
        if (!token) {
            if constexpr (requires(const Promise& promise) { promise.cancellationToken(); }) {
                token = handle.promise().cancellationToken();
            }
        }
        const auto deadline = mini::base::now() + duration_;
        state->handle = handle;
        state->phase = SleepState::Phase::Pending;

        // Everything needed from the awaiter/promise was copied before publishing.
        // Off-thread execution may resume/destroy the frame before this call returns.
        state->loop->runInLoop([state, token = std::move(token), deadline] {
            if (state->phase != SleepState::Phase::Pending) {
                return; // frame was destroyed before queued arming ran
            }
            if (token.isCancellationRequested()) {
                state->loop->queueInLoop([state] { complete(state, SleepState::Phase::Cancelled); });
                return;
            }
            if (token) {
                state->registration.emplace(token.registerCallback(
                    [weak = std::weak_ptr<SleepState>(state)] {
                        if (auto waiting = weak.lock()) {
                            waiting->loop->queueInLoop([waiting] {
                                complete(waiting, SleepState::Phase::Cancelled);
                            });
                        }
                    }));
            }
            state->timerId = state->loop->runAt(deadline, [state] {
                complete(state, SleepState::Phase::Expired);
            });
        });
    }

    mini::net::Expected<void> await_resume() const noexcept {
        if (state_->phase == SleepState::Phase::Cancelled) {
            return std::unexpected(mini::net::NetError::Cancelled);
        }
        return {};
    }

    /// Request completion on the owner loop. The awaitable must still be alive.
    /// Concurrent callers should normally hold a CancellationSource instead.
    void cancel() {
        auto state = state_;
        if (state) {
            state->loop->queueInLoop([state] { complete(state, SleepState::Phase::Cancelled); });
        }
    }

    std::shared_ptr<const SleepState> state() const { return state_; }

private:
    static void complete(const std::shared_ptr<SleepState>& state, SleepState::Phase outcome) {
        state->loop->assertInLoopThread();
        if (state->phase != SleepState::Phase::Pending) {
            return;
        }
        state->phase = outcome;
        auto handle = std::exchange(state->handle, {});
        state->registration.reset();
        state->loop->cancel(std::exchange(state->timerId, {}));
        handle.resume(); // no access to the frame after publication/resumption
    }

    void abandon() noexcept {
        if (!state_ || state_->phase != SleepState::Phase::Pending) {
            return;
        }
        // As with Channel destruction, unregistering a suspended frame is owner-only.
        state_->loop->assertInLoopThread();
        state_->phase = SleepState::Phase::Abandoned;
        state_->handle = {};
        state_->registration.reset();
        state_->loop->cancel(std::exchange(state_->timerId, {}));
    }

    std::shared_ptr<SleepState> state_;
    Duration duration_;
    CancellationToken token_;
};

inline SleepAwaitable asyncSleep(
    mini::net::EventLoop* loop,
    SleepAwaitable::Duration duration,
    CancellationToken token = {}) {
    return SleepAwaitable(loop, duration, std::move(token));
}

} // namespace mini::coroutine
