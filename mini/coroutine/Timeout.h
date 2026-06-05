#pragma once

// Timeout helpers lift the common "operation vs timer" race into the shared
// Expected<T, NetError> surface while preserving existing awaitable APIs.

#include "mini/coroutine/CancellationToken.h"
#include "mini/coroutine/SleepAwaitable.h"
#include "mini/coroutine/Task.h"
#include "mini/coroutine/WhenAny.h"
#include "mini/net/EventLoop.h"
#include "mini/net/NetError.h"

#include <chrono>
#include <coroutine>
#include <expected>
#include <utility>

namespace mini::coroutine {

namespace detail {

class CurrentCancellationTokenAwaitable {
public:
    bool await_ready() const noexcept {
        return false;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        if constexpr (requires(const Promise& promise) { promise.cancellationToken(); }) {
            token_ = handle.promise().cancellationToken();
        } else {
            token_ = {};
        }
        return false;
    }

    CancellationToken await_resume() const noexcept {
        return token_;
    }

private:
    CancellationToken token_{};
};

template <typename T>
Task<mini::net::Expected<T>> timeoutResultTask(
    mini::net::EventLoop* loop,
    std::chrono::steady_clock::duration timeout) {
    auto result = co_await asyncSleep(loop, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return std::unexpected(mini::net::NetError::TimedOut);
}

template <typename T>
Task<mini::net::Expected<T>> prepareTimedOperation(
    Task<mini::net::Expected<T>> operation,
    CancellationToken inheritedToken) {
    LinkedCancellation linkedCancellation;
    linkedCancellation.link(operation.cancellationToken());
    linkedCancellation.link(inheritedToken);
    if (linkedCancellation.linked()) {
        operation.setCancellationToken(linkedCancellation.token());
    }
    co_return co_await std::move(operation);
}

}  // namespace detail

template <typename T>
Task<mini::net::Expected<T>> withTimeout(
    mini::net::EventLoop* loop,
    Task<mini::net::Expected<T>> operation,
    std::chrono::steady_clock::duration timeout) {
    auto inheritedToken = co_await detail::CurrentCancellationTokenAwaitable{};
    auto timeoutTask = detail::timeoutResultTask<T>(loop, timeout);
    if (inheritedToken) {
        timeoutTask.setCancellationToken(inheritedToken);
    }
    auto result = co_await whenAny(
        detail::prepareTimedOperation<T>(std::move(operation), inheritedToken),
        std::move(timeoutTask));
    co_return std::move(result.value);
}

}  // namespace mini::coroutine
