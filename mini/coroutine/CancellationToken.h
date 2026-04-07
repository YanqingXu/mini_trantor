#pragma once

// CancellationToken / CancellationSource 提供协作式取消原语。
// 它们不引入独立调度器，只负责传播“已取消”状态并触发注册回调。

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mini::coroutine {

namespace detail {

struct CancellationState {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::size_t nextCallbackId{1};
    std::unordered_map<std::size_t, std::function<void()>> callbacks;
};

}  // namespace detail

class CancellationRegistration {
public:
    CancellationRegistration() = default;

    CancellationRegistration(
        std::shared_ptr<detail::CancellationState> state,
        std::size_t callbackId) noexcept
        : state_(std::move(state)), callbackId_(callbackId) {
    }

    CancellationRegistration(const CancellationRegistration&) = delete;
    CancellationRegistration& operator=(const CancellationRegistration&) = delete;

    CancellationRegistration(CancellationRegistration&& other) noexcept
        : state_(std::exchange(other.state_, {})),
          callbackId_(std::exchange(other.callbackId_, 0)) {
    }

    CancellationRegistration& operator=(CancellationRegistration&& other) noexcept {
        if (this != &other) {
            reset();
            state_ = std::exchange(other.state_, {});
            callbackId_ = std::exchange(other.callbackId_, 0);
        }
        return *this;
    }

    ~CancellationRegistration() {
        reset();
    }

    void reset() noexcept {
        if (!state_ || callbackId_ == 0) {
            return;
        }

        auto state = std::move(state_);
        const auto callbackId = std::exchange(callbackId_, 0);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->callbacks.erase(callbackId);
        }
    }

private:
    std::shared_ptr<detail::CancellationState> state_;
    std::size_t callbackId_{0};
};

class CancellationToken {
public:
    CancellationToken() = default;

    explicit CancellationToken(std::shared_ptr<detail::CancellationState> state) noexcept
        : state_(std::move(state)) {
    }

    bool isCancellationRequested() const noexcept {
        return state_ && state_->cancelled.load(std::memory_order_acquire);
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(state_);
    }

    CancellationRegistration registerCallback(std::function<void()> callback) const {
        if (!state_) {
            return {};
        }

        bool invokeImmediately = false;
        std::size_t callbackId = 0;

        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->cancelled.load(std::memory_order_acquire)) {
                invokeImmediately = true;
            } else {
                callbackId = state_->nextCallbackId++;
                state_->callbacks.emplace(callbackId, std::move(callback));
            }
        }

        if (invokeImmediately) {
            callback();
            return {};
        }

        return CancellationRegistration(state_, callbackId);
    }

private:
    std::shared_ptr<detail::CancellationState> state_;
};

class CancellationSource {
public:
    CancellationSource()
        : state_(std::make_shared<detail::CancellationState>()) {
    }

    CancellationToken token() const noexcept {
        return CancellationToken(state_);
    }

    bool isCancellationRequested() const noexcept {
        return state_->cancelled.load(std::memory_order_acquire);
    }

    void cancel() const {
        if (!state_) {
            return;
        }

        if (state_->cancelled.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        std::unordered_map<std::size_t, std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            callbacks.swap(state_->callbacks);
        }

        for (auto& [_, callback] : callbacks) {
            callback();
        }
    }

private:
    std::shared_ptr<detail::CancellationState> state_;
};

namespace detail {

class LinkedCancellation {
public:
    void link(const CancellationToken& token) {
        if (!token) {
            return;
        }
        linked_ = true;
        registrations_.push_back(token.registerCallback([source = source_] {
            source.cancel();
        }));
    }

    CancellationToken token() const noexcept {
        return source_.token();
    }

    bool linked() const noexcept {
        return linked_;
    }

private:
    CancellationSource source_{};
    bool linked_{false};
    std::vector<CancellationRegistration> registrations_{};
};

}  // namespace detail

}  // namespace mini::coroutine
