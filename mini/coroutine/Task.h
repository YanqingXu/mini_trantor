#pragma once

// Task 是一个最小可组合的 coroutine 结果对象。
// 它提供 start/detach/co_await 语义，但不替代 EventLoop 的调度规则。

#include "mini/coroutine/CancellationToken.h"
#include "mini/coroutine/ResumeHandle.h"

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

struct TaskAccess;

template <typename T>
class TaskPromiseBase {
public:
    ~TaskPromiseBase() { frameControl_->handle = {}; }

    std::shared_ptr<FrameControl> frameControl() const noexcept { return frameControl_; }

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
                promise.frameControl_->handle = {};
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

    bool try_start() noexcept {
        if (startState_ == StartState::Started) {
            return false;
        }
        startState_ = StartState::Started;
        return true;
    }

    void rethrow_if_exception() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

    void set_cancellation_token(CancellationToken token) noexcept {
        cancellationToken_ = std::move(token);
    }

    CancellationToken cancellationToken() const noexcept {
        return cancellationToken_;
    }

private:
    std::shared_ptr<FrameControl> frameControl_ = std::make_shared<FrameControl>();
    enum class StartState { Lazy, Started };
    StartState startState_{StartState::Lazy};
    std::exception_ptr exception_;
    std::coroutine_handle<> continuation_{};
    bool detached_{false};
    CancellationToken cancellationToken_{};

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

    explicit Task(handle_type coroutine) noexcept
        : coroutine_(coroutine),
          control_(coroutine ? coroutine.promise().frameControl() : nullptr) {
        if (control_) { control_->handle = coroutine; }
    }

    Task(Task&& other) noexcept : coroutine_(std::exchange(other.coroutine_, {})),
                                 control_(std::move(other.control_)) {
    }

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            detail::destroyFrame(coroutine_, control_);
            coroutine_ = std::exchange(other.coroutine_, {});
            control_ = std::move(other.control_);
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() {
        detail::destroyFrame(coroutine_, control_);
    }

    bool done() const noexcept {
        if (!coroutine_) { return true; }
        auto gate = control_->gate;
        std::lock_guard lock(gate->mutex);
        return coroutine_.done();
    }

    void setCancellationToken(CancellationToken token) noexcept {
        if (coroutine_) {
            auto gate = control_->gate;
            std::lock_guard lock(gate->mutex);
            coroutine_.promise().set_cancellation_token(std::move(token));
        }
    }

    CancellationToken cancellationToken() const noexcept {
        if (!coroutine_) { return {}; }
        auto gate = control_->gate;
        std::lock_guard lock(gate->mutex);
        return coroutine_.promise().cancellationToken();
    }

    void start() {
        if (!coroutine_) { return; }
        auto gate = control_->gate;
        std::lock_guard lock(gate->mutex);
        if (coroutine_ && !coroutine_.done()) {
            if (!coroutine_.promise().try_start()) {
                throw std::logic_error("cannot start an already-started Task");
            }
            detail::FrameExecution execution(*gate);
            coroutine_.resume();
        }
    }

    void detach() {
        if (!coroutine_) {
            return;
        }
        auto coroutine = std::exchange(coroutine_, {});
        auto control = std::move(control_);
        auto gate = control->gate;
        std::lock_guard lock(gate->mutex);
        if (coroutine.done()) {
            detail::destroyFrame(coroutine, control);
            return;
        }
        coroutine.promise().set_detached(true);
        if (coroutine.promise().try_start()) {
            detail::FrameExecution execution(*gate);
            coroutine.resume();
        }
    }

    decltype(auto) result() & {
        if (!coroutine_) { throw std::logic_error("task result requested before completion"); }
        auto gate = control_->gate;
        std::lock_guard lock(gate->mutex);
        if (!coroutine_.done()) {
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
        if (!coroutine_) { throw std::logic_error("task result requested before completion"); }
        auto gate = control_->gate;
        std::lock_guard lock(gate->mutex);
        if (!coroutine_.done()) {
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
        std::shared_ptr<detail::FrameControl> control_;

        Awaiter(handle_type coroutine, std::shared_ptr<detail::FrameControl> control) noexcept
            : coroutine_(coroutine), control_(std::move(control)) {}
        Awaiter(const Awaiter&) = delete;
        Awaiter& operator=(const Awaiter&) = delete;
        Awaiter(Awaiter&& other) noexcept : coroutine_(std::exchange(other.coroutine_, {})),
                                           control_(std::move(other.control_)) {}
        Awaiter& operator=(Awaiter&&) = delete;

        bool await_ready() const noexcept {
            if (!coroutine_) { return true; }
            auto gate = control_->gate;
            std::lock_guard lock(gate->mutex);
            return coroutine_.done();
        }

        template <typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> continuation) noexcept {
            if constexpr (requires { continuation.promise().frameControl(); }) {
                // Adoption follows Task ownership synchronization. Sharing a gate
                // also protects the parent's symmetric continuation from this child.
                auto parentControl = continuation.promise().frameControl();
                parentControl->linkChild(control_);
            }
            auto gate = control_->gate;
            std::lock_guard lock(gate->mutex);
            coroutine_.promise().set_continuation(continuation);
            return coroutine_.promise().try_start() ? std::coroutine_handle<>(coroutine_)
                                                   : std::noop_coroutine();
        }

        decltype(auto) await_resume() {
            if (!coroutine_) {
                throw std::logic_error("cannot await an empty Task");
            }
            if constexpr (std::is_void_v<T>) {
                coroutine_.promise().value();
                return;
            } else {
                return std::move(coroutine_.promise()).value();
            }
        }

        ~Awaiter() {
            detail::destroyFrame(coroutine_, control_);
        }
    };

    Awaiter operator co_await() && noexcept {
        return Awaiter{std::exchange(coroutine_, {}), std::move(control_)};
    }

private:
    friend struct detail::TaskAccess;
    handle_type coroutine_{};
    std::shared_ptr<detail::FrameControl> control_;
};

namespace detail {

// Internal composition hook: share execution synchronization, never frame ownership.
// Call only while ownership/adoption is quiescent, before publishing a child.
struct TaskAccess {
    template <typename T, typename Promise>
    static void joinChain(Task<T>& child, std::coroutine_handle<Promise> parent) {
        if constexpr (requires { parent.promise().frameControl(); }) {
            parent.promise().frameControl()->linkChild(child.control_);
        }
    }
};

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
