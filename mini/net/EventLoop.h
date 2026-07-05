#pragma once

// EventLoop 是单线程 Reactor 调度核心，负责 poll、事件分发与跨线程任务回流。
// 所有 loop-owned 可变状态都必须只在 owner 线程上访问与销毁。

#include "mini/base/MetricsHook.h"
#include "mini/base/Timestamp.h"
#include "mini/base/noncopyable.h"
#include "mini/net/TimerId.h"
#include "mini/net/platform/Wakeup.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mini::net {

class Channel;
class Poller;
class TimerQueue;

class EventLoop : private mini::base::noncopyable {
public:
    using Functor = std::function<void()>;
    using TimerDuration = std::chrono::steady_clock::duration;

    EventLoop();
    ~EventLoop();

    void loop();
    void quit();

    mini::base::Timestamp pollReturnTime() const noexcept;

    void runInLoop(Functor cb);
    void queueInLoop(Functor cb);
    void setEventLoopMetricCallback(EventLoopMetricCallback cb);
    TimerId runAt(mini::base::Timestamp time, Functor cb);
    TimerId runAfter(TimerDuration delay, Functor cb);
    TimerId runEvery(TimerDuration interval, Functor cb);
    void cancel(TimerId timerId);

    void wakeup();
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);
    bool hasChannel(Channel* channel);

    bool isInLoopThread() const noexcept;
    void assertInLoopThread() const;

private:
    struct PendingFunctor {
        Functor functor;
        mini::base::Timestamp enqueuedAt;
    };

    void handleRead(mini::base::Timestamp receiveTime);
    void doPendingFunctors();
    void emitEventLoopMetric(EventLoopMetricSample sample);

    using ChannelList = std::vector<Channel*>;

    bool looping_;
    std::atomic<bool> quit_;
    bool eventHandling_;
    bool callingPendingFunctors_;
    const std::thread::id threadId_;
    mini::base::Timestamp pollReturnTime_;
    std::unique_ptr<Poller> poller_;
    std::unique_ptr<TimerQueue> timerQueue_;
    platform::WakeupFdPair wakeupFds_;
    std::unique_ptr<Channel> wakeupChannel_;
    ChannelList activeChannels_;
    Channel* currentActiveChannel_;
    EventLoopMetricCallback eventLoopMetricCallback_;
    std::atomic<std::size_t> pendingFunctorPeak_;
    std::atomic<std::uint64_t> wakeupCount_;
    mutable std::mutex mutex_;
    std::vector<PendingFunctor> pendingFunctors_;
};

}  // namespace mini::net
