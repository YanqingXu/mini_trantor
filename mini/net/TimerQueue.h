#pragma once

// TimerQueue 为单个 EventLoop 提供 timerfd 驱动的定时任务能力。
// 它维护 one-shot / repeating timer，并确保回调始终在 owner loop 线程执行。

#include "mini/base/Timestamp.h"
#include "mini/base/noncopyable.h"
#include "mini/net/TimerId.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mini::net {

class Channel;
class EventLoop;

class TimerQueue : private mini::base::noncopyable {
public:
    using TimerCallback = std::function<void()>;
    using Duration = std::chrono::steady_clock::duration;

    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    TimerId addTimer(TimerCallback cb, mini::base::Timestamp when, Duration interval = Duration::zero());
    void cancel(TimerId timerId);

private:
    struct Timer {
        Timer(TimerCallback timerCallback, mini::base::Timestamp expirationTime, Duration repeatInterval, std::int64_t id)
            : callback(std::move(timerCallback)),
              expiration(expirationTime),
              interval(repeatInterval),
              sequence(id) {
        }

        bool repeat() const noexcept {
            return interval > Duration::zero();
        }

        TimerCallback callback;
        mini::base::Timestamp expiration;
        Duration interval;
        std::int64_t sequence;
        bool canceled{false};
        bool inQueue{true};
    };

    using TimerPtr = std::shared_ptr<Timer>;
    using TimerKey = std::pair<mini::base::Timestamp, std::int64_t>;
    using TimerMap = std::map<TimerKey, TimerPtr>;

    void addTimerInLoop(TimerPtr timer);
    void cancelInLoop(TimerId timerId);
    void handleRead(mini::base::Timestamp receiveTime);

    bool insert(TimerPtr timer);
    std::vector<TimerPtr> getExpired(mini::base::Timestamp now);
    void reset(const std::vector<TimerPtr>& expired, mini::base::Timestamp now);
    void resetTimerfd(mini::base::Timestamp expiration) const;
    void disarmTimerfd() const;

    EventLoop* loop_;
    const int timerfd_;
    std::unique_ptr<Channel> timerfdChannel_;
    std::atomic<std::int64_t> nextSequence_;
    TimerMap timers_;
    std::unordered_map<std::int64_t, TimerPtr> timersById_;
};

}  // namespace mini::net
