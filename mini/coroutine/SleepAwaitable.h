#pragma once

// SleepAwaitable 是基于 TimerQueue 的协程定时等待桥接。
// 它通过 EventLoop::runAfter 注册一次性定时器，到期后在 owner loop 线程恢复协程。
// 它不是独立调度器，不绕过 EventLoop 调度语义。

#include "mini/net/EventLoop.h"
#include "mini/net/NetError.h"
#include "mini/net/TimerId.h"

#include <chrono>
#include <coroutine>
#include <memory>

namespace mini::coroutine {

/// Shared state between the timer callback and potential cancel.
/// Ensures the coroutine handle is resumed exactly once.
struct SleepState {
    mini::net::EventLoop* loop{nullptr};
    std::coroutine_handle<> handle{};
    mini::net::TimerId timerId{};
    bool resumed{false};
    bool cancelled{false};
};

class SleepAwaitable {
public:
    using Duration = std::chrono::steady_clock::duration;

    SleepAwaitable(mini::net::EventLoop* loop, Duration duration)
        : state_(std::make_shared<SleepState>()), duration_(duration) {
        state_->loop = loop;
    }

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        state_->handle = handle;
        auto state = state_;
        state_->timerId = state_->loop->runAfter(duration_, [state] {
            // Timer fired on owner loop thread.
            if (!state->resumed) {
                state->resumed = true;
                state->handle.resume();
            }
        });
    }

    /// Returns success on timer expiry, or Cancelled if the sleep was cancelled.
    mini::net::Expected<void> await_resume() const noexcept {
        if (state_->cancelled) {
            return std::unexpected(mini::net::NetError::Cancelled);
        }
        return {};
    }

    /// Cancel the pending sleep.
    /// Cancels the timer and resumes the coroutine on the owner loop thread.
    /// Safe to call if already expired (no-op).
    void cancel() {
        auto state = state_;
        state->loop->runInLoop([state] {
            if (state->resumed) {
                return;  // already fired, no-op
            }
            state->resumed = true;
            state->cancelled = true;
            state->loop->cancel(state->timerId);
            // Resume the coroutine so the handle is not leaked.
            state->handle.resume();
        });
    }

    /// Get the shared state for external cancellation coordination.
    std::shared_ptr<SleepState> state() const {
        return state_;
    }

private:
    std::shared_ptr<SleepState> state_;
    Duration duration_;
};

/// Factory function: creates a SleepAwaitable for use with co_await.
///
/// Usage:
///   auto result = co_await mini::coroutine::asyncSleep(loop, 100ms);
///   // result.has_value() on timer expiry; result.error() == Cancelled if cancelled.
inline SleepAwaitable asyncSleep(mini::net::EventLoop* loop, SleepAwaitable::Duration duration) {
    return SleepAwaitable(loop, duration);
}

}  // namespace mini::coroutine
