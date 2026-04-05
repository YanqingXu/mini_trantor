#pragma once

// WhenAll 是协程组合原语，等待多个 Task<T> 全部完成后恢复调用方。
// 结果按输入顺序收集到 std::tuple<Ts...>。全部为 Task<void> 时返回 Task<void>。
// 它是纯协程层构造，不依赖 EventLoop，不绕过 Reactor 调度语义。

#include "mini/coroutine/Task.h"

#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace mini::coroutine {

namespace detail {

// --- Shared control block for WhenAll ---

template <typename... Ts>
struct WhenAllState {
    std::atomic<std::size_t> remaining;
    std::coroutine_handle<> parent{};
    std::exception_ptr firstException{};
    std::tuple<std::optional<Ts>...> results;

    explicit WhenAllState(std::size_t count) : remaining(count) {}

    void captureException(std::exception_ptr e) {
        if (!firstException) {
            firstException = std::move(e);
        }
    }

    void onComplete() {
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // Last sub-task completed — resume parent.
            if (parent) {
                parent.resume();
            }
        }
    }
};

// Specialization for all-void case
struct WhenAllVoidState {
    std::atomic<std::size_t> remaining;
    std::coroutine_handle<> parent{};
    std::exception_ptr firstException{};

    explicit WhenAllVoidState(std::size_t count) : remaining(count) {}

    void captureException(std::exception_ptr e) {
        if (!firstException) {
            firstException = std::move(e);
        }
    }

    void onComplete() {
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (parent) {
                parent.resume();
            }
        }
    }
};

// --- Wrapper coroutine: runs one sub-task and stores result ---

template <std::size_t I, typename T, typename State>
Task<void> whenAllWrapper(Task<T> subtask, std::shared_ptr<State> state) {
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(subtask);
        } else {
            auto val = co_await std::move(subtask);
            std::get<I>(state->results).emplace(std::move(val));
        }
    } catch (...) {
        state->captureException(std::current_exception());
    }
    state->onComplete();
}

// --- WhenAll Awaitable (value types) ---

template <typename... Ts>
class WhenAllAwaitable {
public:
    explicit WhenAllAwaitable(Task<Ts>&&... tasks)
        : state_(std::make_shared<WhenAllState<Ts...>>(sizeof...(Ts))) {
        initWrappers(std::index_sequence_for<Ts...>{}, std::move(tasks)...);
    }

    bool await_ready() const noexcept { return sizeof...(Ts) == 0; }

    void await_suspend(std::coroutine_handle<> parent) {
        state_->parent = parent;
        // Start all wrapper coroutines (detach = fire-and-forget).
        for (auto& w : wrappers_) {
            w.detach();
        }
    }

    std::tuple<Ts...> await_resume() {
        if (state_->firstException) {
            std::rethrow_exception(state_->firstException);
        }
        return extractResults(std::index_sequence_for<Ts...>{});
    }

private:
    template <std::size_t... Is>
    void initWrappers(std::index_sequence<Is...>, Task<Ts>&&... tasks) {
        ((wrappers_[Is] = whenAllWrapper<Is, Ts, WhenAllState<Ts...>>(
              std::move(tasks), state_)),
         ...);
    }

    template <std::size_t... Is>
    std::tuple<Ts...> extractResults(std::index_sequence<Is...>) {
        return std::tuple<Ts...>{std::move(*std::get<Is>(state_->results))...};
    }

    std::shared_ptr<WhenAllState<Ts...>> state_;
    Task<void> wrappers_[sizeof...(Ts)];
};

// --- WhenAll Awaitable (all void) ---

template <std::size_t N>
class WhenAllVoidAwaitable {
public:
    template <typename... Vs>
    explicit WhenAllVoidAwaitable(Task<Vs>&&... tasks)
        : state_(std::make_shared<WhenAllVoidState>(N)) {
        static_assert(sizeof...(Vs) == N);
        std::size_t i = 0;
        ((wrappers_[i++] = whenAllVoidWrapper(std::move(tasks), state_)), ...);
    }

    bool await_ready() const noexcept { return N == 0; }

    void await_suspend(std::coroutine_handle<> parent) {
        state_->parent = parent;
        for (std::size_t i = 0; i < N; ++i) {
            wrappers_[i].detach();
        }
    }

    void await_resume() {
        if (state_->firstException) {
            std::rethrow_exception(state_->firstException);
        }
    }

private:
    static Task<void> whenAllVoidWrapper(Task<void> subtask,
                                          std::shared_ptr<WhenAllVoidState> state) {
        try {
            co_await std::move(subtask);
        } catch (...) {
            state->captureException(std::current_exception());
        }
        state->onComplete();
    }

    std::shared_ptr<WhenAllVoidState> state_;
    Task<void> wrappers_[N > 0 ? N : 1];
};

}  // namespace detail

// --- Public API ---

/// whenAll(Task<T1>, Task<T2>, ...) → Task<std::tuple<T1, T2, ...>>
/// All Ts must be non-void. For all-void, see the void overload below.
/// Parameters taken by value so they are moved into the coroutine frame
/// and survive across the initial suspension point.
template <typename... Ts>
requires(sizeof...(Ts) > 0 && (!std::is_void_v<Ts> && ...))
Task<std::tuple<Ts...>> whenAll(Task<Ts>... tasks) {
    co_return co_await detail::WhenAllAwaitable<Ts...>(std::move(tasks)...);
}

/// whenAll(Task<void>, ...) → Task<void>
/// All sub-tasks must be Task<void>.
template <typename... Vs>
requires(sizeof...(Vs) > 0 && (std::is_void_v<Vs> && ...))
Task<void> whenAll(Task<Vs>... tasks) {
    co_await detail::WhenAllVoidAwaitable<sizeof...(Vs)>(std::move(tasks)...);
}

/// whenAll() with zero tasks → Task<void>, completes immediately.
inline Task<void> whenAll() {
    co_return;
}

}  // namespace mini::coroutine
