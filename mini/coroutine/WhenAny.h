#pragma once

// WhenAny 是协程组合原语，等待多个同类型 Task<T> 中第一个完成后恢复调用方。
// 返回 WhenAnyResult<T>，包含获胜 sub-task 的 index 和 value（void 特化只有 index）。
// 剩余 sub-task 的 wrapper coroutine 仍会运行完成并被正确销毁。
// 它是纯协程层构造，不依赖 EventLoop，不绕过 Reactor 调度语义。

#include "mini/coroutine/Task.h"

#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace mini::coroutine {

/// Result type for WhenAny with value-returning tasks.
template <typename T>
struct WhenAnyResult {
    std::size_t index;
    T value;
};

/// Result type for WhenAny with void tasks.
template <>
struct WhenAnyResult<void> {
    std::size_t index;
};

namespace detail {

// --- Shared control block for WhenAny (value type) ---
// Shared state does not own wrapper frames: wrappers themselves retain it.
// await_suspend moves launch ownership into locals before any child can win.

template <typename T, std::size_t N>
struct WhenAnyState {
    std::atomic<bool> done{false};
    ResumeHandle parent{};
    std::size_t winnerIndex{0};
    std::optional<T> winnerValue{};
    std::exception_ptr winnerException{};
    std::array<CancellationSource, N> cancellationSources{};
    std::array<LinkedCancellation, N> linkedCancellations{};

    bool tryWin(std::size_t index) {
        bool expected = false;
        if (done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            winnerIndex = index;
            return true;
        }
        return false;
    }

    void resumeParent() {
        if (parent) {
            parent.resume();
        }
    }

    void cancelLosers(std::size_t winner) noexcept {
        for (std::size_t i = 0; i < N; ++i) {
            if (i != winner) {
                try { cancellationSources[i].cancel(); }
                catch (...) {
                    if (!winnerException) { winnerException = std::current_exception(); }
                }
            }
        }
    }
};

// --- Shared control block for WhenAny (void) ---

template <std::size_t N>
struct WhenAnyVoidState {
    std::atomic<bool> done{false};
    ResumeHandle parent{};
    std::size_t winnerIndex{0};
    std::exception_ptr winnerException{};
    std::array<CancellationSource, N> cancellationSources{};
    std::array<LinkedCancellation, N> linkedCancellations{};

    bool tryWin(std::size_t index) {
        bool expected = false;
        if (done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            winnerIndex = index;
            return true;
        }
        return false;
    }

    void resumeParent() {
        if (parent) {
            parent.resume();
        }
    }

    void cancelLosers(std::size_t winner) noexcept {
        for (std::size_t i = 0; i < N; ++i) {
            if (i != winner) {
                try { cancellationSources[i].cancel(); }
                catch (...) {
                    if (!winnerException) { winnerException = std::current_exception(); }
                }
            }
        }
    }
};

// --- Wrapper coroutine for value-returning sub-task ---

template <typename T, std::size_t N>
Task<void> whenAnyValueWrapper(Task<T> subtask, std::size_t index,
                                std::shared_ptr<WhenAnyState<T, N>> state) {
    try {
        auto val = co_await std::move(subtask);
        if (state->tryWin(index)) {
            // Winning the race is terminal even when moving T into storage fails.
            try {
                state->winnerValue.emplace(std::move(val));
            } catch (...) {
                state->winnerException = std::current_exception();
            }
            state->cancelLosers(index);
            state->resumeParent();
        }
    } catch (...) {
        if (state->tryWin(index)) {
            state->winnerException = std::current_exception();
            state->cancelLosers(index);
            state->resumeParent();
        }
        // If not winner, exception is silently discarded.
    }
}

// --- Wrapper coroutine for void sub-task ---

template <std::size_t N>
Task<void> whenAnyVoidWrapper(Task<void> subtask, std::size_t index,
                               std::shared_ptr<WhenAnyVoidState<N>> state) {
    try {
        co_await std::move(subtask);
        if (state->tryWin(index)) {
            state->cancelLosers(index);
            state->resumeParent();
        }
    } catch (...) {
        if (state->tryWin(index)) {
            state->winnerException = std::current_exception();
            state->cancelLosers(index);
            state->resumeParent();
        }
    }
}

// --- WhenAny Awaitable (value type) ---

template <typename T, std::size_t N>
class WhenAnyAwaitable {
public:
    template <typename... Tasks>
    explicit WhenAnyAwaitable(Task<Tasks>&&... tasks)
        : state_(std::make_shared<WhenAnyState<T, N>>()) {
        static_assert(sizeof...(Tasks) == N);
        std::size_t i = 0;
        ((state_->linkedCancellations[i].link(tasks.cancellationToken()),
          state_->linkedCancellations[i].link(state_->cancellationSources[i].token()),
          tasks.setCancellationToken(state_->linkedCancellations[i].token()),
          wrappers_[i] = whenAnyValueWrapper<T, N>(std::move(tasks), i, state_),
          ++i),
         ...);
    }

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> parent) {
        auto state = state_;
        auto wrappers = std::move(wrappers_);
        state->parent = borrowResume(parent);
        for (auto& wrapper : wrappers) { TaskAccess::joinChain(wrapper, parent); }
        for (auto& wrapper : wrappers) {
            wrapper.detach();
        }
    }

    WhenAnyResult<T> await_resume() {
        if (state_->winnerException) {
            std::rethrow_exception(state_->winnerException);
        }
        return WhenAnyResult<T>{state_->winnerIndex,
                                std::move(*state_->winnerValue)};
    }

private:
    std::shared_ptr<WhenAnyState<T, N>> state_;
    std::array<Task<void>, N> wrappers_;
};

// --- WhenAny Awaitable (void) ---

template <std::size_t N>
class WhenAnyVoidAwaitable {
public:
    template <typename... Vs>
    explicit WhenAnyVoidAwaitable(Task<Vs>&&... tasks)
        : state_(std::make_shared<WhenAnyVoidState<N>>()) {
        static_assert(sizeof...(Vs) == N);
        std::size_t i = 0;
        ((state_->linkedCancellations[i].link(tasks.cancellationToken()),
          state_->linkedCancellations[i].link(state_->cancellationSources[i].token()),
          tasks.setCancellationToken(state_->linkedCancellations[i].token()),
          wrappers_[i] = whenAnyVoidWrapper<N>(std::move(tasks), i, state_),
          ++i),
         ...);
    }

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> parent) {
        auto state = state_;
        auto wrappers = std::move(wrappers_);
        state->parent = borrowResume(parent);
        for (auto& wrapper : wrappers) { TaskAccess::joinChain(wrapper, parent); }
        for (auto& wrapper : wrappers) {
            wrapper.detach();
        }
    }

    WhenAnyResult<void> await_resume() {
        if (state_->winnerException) {
            std::rethrow_exception(state_->winnerException);
        }
        return WhenAnyResult<void>{state_->winnerIndex};
    }

private:
    std::shared_ptr<WhenAnyVoidState<N>> state_;
    std::array<Task<void>, N> wrappers_;
};

}  // namespace detail

// --- Public API ---

/// whenAny(Task<T>, Task<T>, ...) → Task<WhenAnyResult<T>>
/// All sub-tasks must return the same non-void type T.
/// Parameters taken by value so they are moved into the coroutine frame
/// and survive across the initial suspension point.
template <typename T, typename... Rest>
requires(sizeof...(Rest) >= 0 && (std::same_as<T, Rest> && ...) && !std::is_void_v<T>)
Task<WhenAnyResult<T>> whenAny(Task<T> first, Task<Rest>... rest) {
    co_return co_await detail::WhenAnyAwaitable<T, 1 + sizeof...(Rest)>(
        std::move(first), std::move(rest)...);
}

/// whenAny(Task<void>, ...) → Task<WhenAnyResult<void>>
/// All sub-tasks must be Task<void>.
template <typename... Vs>
requires(sizeof...(Vs) > 0 && (std::is_void_v<Vs> && ...))
Task<WhenAnyResult<void>> whenAny(Task<Vs>... tasks) {
    co_return co_await detail::WhenAnyVoidAwaitable<sizeof...(Vs)>(
        std::move(tasks)...);
}

}  // namespace mini::coroutine
