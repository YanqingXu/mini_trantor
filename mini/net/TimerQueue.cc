#include "mini/net/TimerQueue.h"

#include "mini/net/Channel.h"
#include "mini/net/EventLoop.h"

#include "mini/base/Logger.h"

#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <sys/timerfd.h>
#include <unistd.h>

namespace mini::net {

namespace {

int createTimerfd() {
    const int timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd < 0) {
        LOG_SYSFATAL << "timerfd_create: " << std::strerror(errno);
    }
    return timerfd;
}

timespec toTimespec(std::chrono::steady_clock::duration delay) {
    using namespace std::chrono;

    if (delay < 1us) {
        delay = 1us;
    }

    const auto secondsPart = duration_cast<seconds>(delay);
    const auto nanosecondsPart = duration_cast<nanoseconds>(delay - secondsPart);

    timespec ts{};
    ts.tv_sec = static_cast<time_t>(secondsPart.count());
    ts.tv_nsec = static_cast<long>(nanosecondsPart.count());
    return ts;
}

void readTimerfdOrDie(int timerfd) {
    std::uint64_t expirations = 0;
    const ssize_t n = ::read(timerfd, &expirations, sizeof(expirations));
    if (n == static_cast<ssize_t>(sizeof(expirations)) || errno == EAGAIN) {
        return;
    }
    LOG_SYSFATAL << "TimerQueue::handleRead: " << std::strerror(errno);
}

}  // namespace

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop),
      timerfd_(createTimerfd()),
      timerfdChannel_(std::make_unique<Channel>(loop, timerfd_)),
      nextSequence_(1) {
    timerfdChannel_->setReadCallback([this](mini::base::Timestamp receiveTime) { handleRead(receiveTime); });
    timerfdChannel_->enableReading();
}

TimerQueue::~TimerQueue() {
    loop_->assertInLoopThread();
    timerfdChannel_->disableAll();
    timerfdChannel_->remove();
    ::close(timerfd_);
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

        if (timers_.empty()) {
            disarmTimerfd();
        } else {
            resetTimerfd(timers_.begin()->second->expiration);
        }
        return;
    }

    timersById_.erase(found);
}

void TimerQueue::handleRead(mini::base::Timestamp receiveTime) {
    loop_->assertInLoopThread();
    readTimerfdOrDie(timerfd_);

    auto expired = getExpired(receiveTime);
    for (const auto& timer : expired) {
        if (!timer->canceled) {
            timer->callback();
        }
    }
    reset(expired, receiveTime);
}

bool TimerQueue::insert(TimerPtr timer) {
    loop_->assertInLoopThread();

    const bool earliestChanged = timers_.empty() || TimerKey{timer->expiration, timer->sequence} < timers_.begin()->first;
    timer->inQueue = true;
    timer->canceled = false;
    timersById_[timer->sequence] = timer;
    timers_.emplace(TimerKey{timer->expiration, timer->sequence}, timer);

    if (earliestChanged) {
        resetTimerfd(timer->expiration);
    }

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

    if (timers_.empty()) {
        disarmTimerfd();
    } else {
        resetTimerfd(timers_.begin()->second->expiration);
    }
}

void TimerQueue::resetTimerfd(mini::base::Timestamp expiration) const {
    const auto delay = expiration - mini::base::now();

    itimerspec newValue{};
    newValue.it_value = toTimespec(delay);

    if (::timerfd_settime(timerfd_, 0, &newValue, nullptr) < 0) {
        LOG_SYSFATAL << "TimerQueue::resetTimerfd: " << std::strerror(errno);
    }
}

void TimerQueue::disarmTimerfd() const {
    const itimerspec disarmed{};
    if (::timerfd_settime(timerfd_, 0, &disarmed, nullptr) < 0) {
        LOG_SYSFATAL << "TimerQueue::disarmTimerfd: " << std::strerror(errno);
    }
}

}  // namespace mini::net
