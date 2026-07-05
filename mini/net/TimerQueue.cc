#include "mini/net/TimerQueue.h"

#include "mini/net/EventLoop.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace mini::net {

namespace {

int ceilMilliseconds(std::chrono::steady_clock::duration delay) {
    using namespace std::chrono;

    if (delay <= steady_clock::duration::zero()) {
        return 0;
    }

    auto millisecondsPart = duration_cast<milliseconds>(delay);
    if (millisecondsPart < delay) {
        ++millisecondsPart;
    }
    const auto capped = std::min<std::int64_t>(
        millisecondsPart.count(),
        std::numeric_limits<int>::max());
    return static_cast<int>(std::max<std::int64_t>(1, capped));
}

}  // namespace

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop),
      nextSequence_(1) {
}

TimerQueue::~TimerQueue() {
    loop_->assertInLoopThread();
}

TimerId TimerQueue::addTimer(TimerCallback cb, mini::base::Timestamp when, Duration interval) {
    auto timer = std::make_shared<Timer>(std::move(cb), when, interval, nextSequence_.fetch_add(1));
    const TimerId timerId(timer->sequence);

    if (loop_->isInLoopThread()) {
        addTimerInLoop(std::move(timer));
    } else {
        loop_->runInLoop([this, timer] { addTimerInLoop(timer); });
    }

    return timerId;
}

void TimerQueue::cancel(TimerId timerId) {
    if (!timerId.valid()) {
        return;
    }

    if (loop_->isInLoopThread()) {
        cancelInLoop(timerId);
    } else {
        loop_->runInLoop([this, timerId] { cancelInLoop(timerId); });
    }
}

void TimerQueue::addTimerInLoop(TimerPtr timer) {
    loop_->assertInLoopThread();
    insert(std::move(timer));
}

void TimerQueue::cancelInLoop(TimerId timerId) {
    loop_->assertInLoopThread();

    const auto found = timersById_.find(timerId.sequence_);
    if (found == timersById_.end()) {
        return;
    }

    const auto& timer = found->second;
    timer->canceled = true;

    if (timer->inQueue) {
        timers_.erase(TimerKey{timer->expiration, timer->sequence});
        timer->inQueue = false;
        timersById_.erase(found);

        return;
    }

    timersById_.erase(found);
}

int TimerQueue::pollTimeoutMs(int defaultTimeoutMs) const {
    loop_->assertInLoopThread();
    if (timers_.empty()) {
        return defaultTimeoutMs;
    }

    const int nextTimerMs = ceilMilliseconds(timers_.begin()->second->expiration - mini::base::now());
    return std::min(defaultTimeoutMs, nextTimerMs);
}

void TimerQueue::handleExpired(mini::base::Timestamp now) {
    loop_->assertInLoopThread();
    if (timers_.empty() || timers_.begin()->second->expiration > now) {
        return;
    }

    auto expired = getExpired(now);
    for (const auto& timer : expired) {
        if (!timer->canceled) {
            timer->callback();
        }
    }
    reset(expired, mini::base::now());
}

bool TimerQueue::insert(TimerPtr timer) {
    loop_->assertInLoopThread();

    const bool earliestChanged = timers_.empty() || TimerKey{timer->expiration, timer->sequence} < timers_.begin()->first;
    timer->inQueue = true;
    timer->canceled = false;
    timersById_[timer->sequence] = timer;
    timers_.emplace(TimerKey{timer->expiration, timer->sequence}, timer);

    return earliestChanged;
}

std::vector<TimerQueue::TimerPtr> TimerQueue::getExpired(mini::base::Timestamp now) {
    std::vector<TimerPtr> expired;
    const TimerKey sentry{now, std::numeric_limits<std::int64_t>::max()};
    const auto end = timers_.upper_bound(sentry);

    for (auto it = timers_.begin(); it != end; ++it) {
        it->second->inQueue = false;
        expired.push_back(it->second);
    }

    timers_.erase(timers_.begin(), end);
    return expired;
}

void TimerQueue::reset(const std::vector<TimerPtr>& expired, mini::base::Timestamp now) {
    for (const auto& timer : expired) {
        if (timer->repeat() && !timer->canceled && timersById_.contains(timer->sequence)) {
            timer->expiration = now + timer->interval;
            insert(timer);
            continue;
        }
        timersById_.erase(timer->sequence);
    }

}

}  // namespace mini::net
