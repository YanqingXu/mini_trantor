#pragma once

// Task 是一个最小可组合的 coroutine 结果对象。
// 它提供 start/detach/co_await 语义，但不替代 EventLoop 的调度规则。

#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace mini::coroutine {

template <typename T>
class Task;

namespace detail {

template <typename T>
class TaskPromiseBase {
public:
    std::suspend_always initial_suspend() noexcept {
        return {};
    }

    struct FinalAwaiter {
        bool await_ready() const noexcept {
            return false;
        }

        template <typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) const noexcept {
            auto& promise = handle.promise();
            if (promise.continuation_) {
                return promise.continuation_;
            }
            if (promise.detached_) {
                handle.destroy();
            }
            return std::noop_coroutine();
        }

        void await_resume() const noexcept {
        }
    };

    FinalAwaiter final_suspend() noexcept {
        return {};
    }

    void unhandled_exception() {
        exception_ = std::current_exception();
    }

    void set_detached(bool detached) noexcept {
        detached_ = detached;
    }

    bool detached() const noexcept {
        return detached_;
    }

    void set_continuation(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
    }

    void rethrow_if_exception() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

private:
    std::exception_ptr exception_;
    std::coroutine_handle<> continuation_{};
    bool detached_{false};

    template <typename U>
    friend class ::mini::coroutine::Task;
};

template <typename T>
class TaskPromise final : public TaskPromiseBase<T> {
public:
    Task<T> get_return_object() noexcept;

    template <typename U>
    requires std::convertible_to<U, T>
    void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
        value_.emplace(std::forward<U>(value));
    }

    T& value() & {
        this->rethrow_if_exception();
        if (!value_) {
            throw std::logic_error("task value is not ready");
        }
        return *value_;
    }

    T&& value() && {
        this->rethrow_if_exception();
        if (!value_) {
            throw std::logic_error("task value is not ready");
        }
        return std::move(*value_);
    }

private:
    std::optional<T> value_;
};

template <>
class TaskPromise<void> final : public TaskPromiseBase<void> {
public:
    Task<void> get_return_object() noexcept;

    void return_void() noexcept {
    }

    void value() {
        this->rethrow_if_exception();
    }
};

}  // namespace detail

template <typename T>
class Task {
public:
    using promise_type = detail::TaskPromise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    Task() = default;

    explicit Task(handle_type coroutine) noexcept : coroutine_(coroutine) {
    }

    Task(Task&& other) noexcept : coroutine_(std::exchange(other.coroutine_, {})) {
    }

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (coroutine_) {
                coroutine_.destroy();
            }
            coroutine_ = std::exchange(other.coroutine_, {});
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() {
        if (coroutine_) {
            coroutine_.destroy();
        }
    }

    bool done() const noexcept {
        return !coroutine_ || coroutine_.done();
    }

    void start() {
        if (coroutine_ && !coroutine_.done()) {
            coroutine_.resume();
        }
    }

    void detach() {
        if (!coroutine_) {
            return;
        }
        auto coroutine = std::exchange(coroutine_, {});
        coroutine.promise().set_detached(true);
        coroutine.resume();
    }

    decltype(auto) result() & {
        if (!coroutine_ || !coroutine_.done()) {
            throw std::logic_error("task result requested before completion");
        }
        if constexpr (std::is_void_v<T>) {
            coroutine_.promise().value();
            return;
        } else {
            return coroutine_.promise().value();
        }
    }

    decltype(auto) result() && {
        if (!coroutine_ || !coroutine_.done()) {
            throw std::logic_error("task result requested before completion");
        }
        if constexpr (std::is_void_v<T>) {
            coroutine_.promise().value();
            return;
        } else {
            return std::move(coroutine_.promise()).value();
        }
    }

    struct Awaiter {
        handle_type coroutine_;

        bool await_ready() const noexcept {
            return !coroutine_ || coroutine_.done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
            coroutine_.promise().set_continuation(continuation);
            return coroutine_;
        }

        decltype(auto) await_resume() {
            if constexpr (std::is_void_v<T>) {
                coroutine_.promise().value();
                return;
            } else {
                return std::move(coroutine_.promise()).value();
            }
        }

        ~Awaiter() {
            if (coroutine_) {
                coroutine_.destroy();
            }
        }
    };

    Awaiter operator co_await() && noexcept {
        return Awaiter{std::exchange(coroutine_, {})};
    }

private:
    handle_type coroutine_{};
};

namespace detail {

template <typename T>
Task<T> TaskPromise<T>::get_return_object() noexcept {
    using Handle = std::coroutine_handle<TaskPromise<T>>;
    return Task<T>(Handle::from_promise(*this));
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    using Handle = std::coroutine_handle<TaskPromise<void>>;
    return Task<void>(Handle::from_promise(*this));
}

}  // namespace detail

}  // namespace mini::coroutine
