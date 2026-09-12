#pragma once

// LoopHandle 不拥有 EventLoop，只通过共享投递状态保护 enqueue 与 wakeup。
// loop 关闭后 queue 返回 false；它不在调用线程执行 fallback callback。
// Intent: intents/modules/loop_handle.intent.md

#include <functional>
#include <memory>
#include <mutex>

namespace mini::net {

class EventLoop;

class LoopHandle {
public:
    using Functor = std::function<void()>;

    LoopHandle() = default;

    /// Always queue on the target owner. False means the target is closed/empty.
    /// Accepted is not completed; a never-run loop may discard accepted work.
    /// Empty callbacks throw invalid_argument. This handle does not own the loop.
    bool queue(Functor callback) const;

private:
    struct State {
        explicit State(EventLoop* owner) : loop(owner) {}
        std::mutex mutex;
        EventLoop* loop;
    };

    explicit LoopHandle(EventLoop* loop) : state_(std::make_shared<State>(loop)) {}
    std::shared_ptr<State> state_;
    friend class EventLoop;
};

} // namespace mini::net
