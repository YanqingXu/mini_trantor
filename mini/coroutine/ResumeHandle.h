#pragma once

// ResumeHandle 只借用 coroutine frame；FrameControl 在所有者析构前失效。
// gate 保护恢复与释放，不拥有 EventLoop，也不改变调度线程。
// Intent: intents/modules/resume_handle.intent.md

#include <concepts>
#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace mini::coroutine::detail {

struct FrameGate {
    std::recursive_mutex mutex;
    std::size_t executions{0};
};

struct FrameControl {
    std::coroutine_handle<> handle{}; // borrowed, never implicitly destroyed
    std::shared_ptr<FrameGate> gate = std::make_shared<FrameGate>();
    std::vector<std::weak_ptr<FrameControl>> children;

    // Task adoption is an externally synchronized ownership operation. A started
    // child may already own a suspended chain; migrate that whole chain as well.
    void adoptGate(const std::shared_ptr<FrameGate>& adoptedGate) {
        gate = adoptedGate;
        for (auto& borrowed : children) {
            if (auto child = borrowed.lock()) { child->adoptGate(adoptedGate); }
        }
    }

    void linkChild(const std::shared_ptr<FrameControl>& child) {
        std::erase_if(children, [](const auto& borrowed) { return borrowed.expired(); });
        child->adoptGate(gate);
        children.emplace_back(child);
    }
};

class FrameExecution {
public:
    explicit FrameExecution(FrameGate& gate) noexcept : gate_(gate) { ++gate_.executions; }
    ~FrameExecution() { --gate_.executions; }
    FrameExecution(const FrameExecution&) = delete;
    FrameExecution& operator=(const FrameExecution&) = delete;
private:
    FrameGate& gate_;
};

inline void destroyFrame(std::coroutine_handle<> handle,
                         const std::shared_ptr<FrameControl>& control) noexcept {
    if (!handle) { return; }
    auto gate = control->gate;
    std::lock_guard lock(gate->mutex);
    // Recursive completion may release a child at final_suspend, but destroying
    // an executing chain before final suspension would invalidate its own stack.
    if (gate->executions != 0 && !handle.done()) { std::terminate(); }
    control->handle = {};
    handle.destroy();
}

class ResumeHandle {
public:
    ResumeHandle() = default;
    explicit ResumeHandle(std::coroutine_handle<> handle,
                           std::shared_ptr<FrameControl> control = {}) noexcept
        : handle_(handle), control_(std::move(control)) {}

    explicit operator bool() const noexcept { return static_cast<bool>(handle_); }

    void resume() const {
        if (!handle_) { return; }
        if (!control_) {
            handle_.resume(); // foreign promises supply their own lifetime contract
            return;
        }
        auto control = control_;
        auto gate = control->gate;
        std::lock_guard lock(gate->mutex);
        if (control->handle != handle_ || handle_.done()) { return; }
        FrameExecution execution(*gate);
        handle_.resume();
    }

private:
    std::coroutine_handle<> handle_{};
    std::shared_ptr<FrameControl> control_;
};

template <typename Promise>
ResumeHandle borrowResume(std::coroutine_handle<Promise> handle) noexcept {
    if constexpr (requires {
        { handle.promise().frameControl() } -> std::same_as<std::shared_ptr<FrameControl>>;
    }) {
        return ResumeHandle(handle, handle.promise().frameControl());
    } else {
        return ResumeHandle(handle);
    }
}

} // namespace mini::coroutine::detail
