#include "mini/net/EventLoop.h"

#include "mini/net/Channel.h"
#include "mini/net/Poller.h"
#include "mini/net/SocketsOps.h"
#include "mini/net/TimerQueue.h"

#include "mini/base/Logger.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#ifndef _WIN32
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace mini::net {

namespace {

thread_local EventLoop* t_loopInThisThread = nullptr;

WakeupFdPair createWakeupFds() {
#ifdef _WIN32
    SocketFd fds[2]{kInvalidSocket, kInvalidSocket};
    sockets::createSocketPairOrDie(fds);
    return {.readFd = fds[0], .writeFd = fds[1]};
#else
    const SocketFd eventfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventfd < 0) {
        LOG_SYSFATAL << "eventfd: " << std::strerror(errno);
    }
    return {.readFd = eventfd, .writeFd = eventfd};
#endif
}

void closeWakeupFds(WakeupFdPair fds) {
    sockets::close(fds.readFd);
    if (fds.writeFd != fds.readFd) {
        sockets::close(fds.writeFd);
    }
}

ssize_t writeWakeup(SocketFd fd) {
#ifdef _WIN32
    const unsigned char one = 1;
    return sockets::write(fd, &one, sizeof(one));
#else
    const uint64_t one = 1;
    return sockets::write(fd, &one, sizeof(one));
#endif
}

bool drainWakeup(SocketFd fd) {
#ifdef _WIN32
    bool drained = false;
    char buffer[64];
    while (true) {
        const ssize_t n = sockets::read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            drained = true;
            continue;
        }
        if (n < 0 && sockets::isWouldBlock(sockets::lastError())) {
            return drained;
        }
        return drained || n == 0;
    }
#else
    uint64_t one = 0;
    const ssize_t n = sockets::read(fd, &one, sizeof(one));
    return n == static_cast<ssize_t>(sizeof(one));
#endif
}

}  // namespace

EventLoop::EventLoop()
    : looping_(false),
      quit_(false),
      eventHandling_(false),
      callingPendingFunctors_(false),
      threadId_(std::this_thread::get_id()),
      poller_(Poller::newDefaultPoller(this)),
      timerQueue_(std::make_unique<TimerQueue>(this)),
      wakeupFds_(createWakeupFds()),
      wakeupChannel_(std::make_unique<Channel>(this, wakeupFds_.readFd)),
      currentActiveChannel_(nullptr),
      pendingFunctorPeak_(0),
      wakeupCount_(0) {
    if (t_loopInThisThread != nullptr) {
        throw std::runtime_error("another EventLoop already exists in this thread");
    }
    t_loopInThisThread = this;

    wakeupChannel_->setReadCallback([this](mini::base::Timestamp receiveTime) { handleRead(receiveTime); });
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop() {
    if (!isInLoopThread()) {
        LOG_FATAL << "EventLoop destroyed from non-owner thread";
    }
    if (looping_) {
        LOG_FATAL << "EventLoop destroyed while loop() is still running";
    }
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    closeWakeupFds(wakeupFds_);
    t_loopInThisThread = nullptr;
}

void EventLoop::loop() {
    assertInLoopThread();
    looping_ = true;

    while (!quit_) {
        activeChannels_.clear();
        pollReturnTime_ = poller_->poll(timerQueue_->pollTimeoutMs(10000), &activeChannels_);
        eventHandling_ = true;
        for (Channel* channel : activeChannels_) {
            currentActiveChannel_ = channel;
            channel->handleEvent(pollReturnTime_);
        }
        currentActiveChannel_ = nullptr;
        eventHandling_ = false;
        timerQueue_->handleExpired(mini::base::now());
        doPendingFunctors();
    }

    while (true) {
        bool hasPending = false;
        {
            std::lock_guard lock(mutex_);
            hasPending = !pendingFunctors_.empty();
        }
        if (!hasPending) {
            break;
        }
        doPendingFunctors();
    }

    looping_ = false;
}

void EventLoop::quit() {
    quit_ = true;
    if (!isInLoopThread()) {
        wakeup();
    }
}

mini::base::Timestamp EventLoop::pollReturnTime() const noexcept {
    return pollReturnTime_;
}

void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) {
        cb();
    } else {
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(Functor cb) {
    const auto enqueuedAt = mini::base::now();
    {
        std::lock_guard lock(mutex_);
        pendingFunctors_.push_back(PendingFunctor{std::move(cb), enqueuedAt});
        const auto pendingSize = pendingFunctors_.size();
        auto observedPeak = pendingFunctorPeak_.load(std::memory_order_relaxed);
        while (pendingSize > observedPeak &&
               !pendingFunctorPeak_.compare_exchange_weak(
                   observedPeak,
                   pendingSize,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
    }

    if (!isInLoopThread() || callingPendingFunctors_) {
        wakeup();
    }
}

void EventLoop::setEventLoopMetricCallback(EventLoopMetricCallback cb) {
    assertInLoopThread();
    eventLoopMetricCallback_ = std::move(cb);
}

TimerId EventLoop::runAt(mini::base::Timestamp time, Functor cb) {
    return timerQueue_->addTimer(std::move(cb), time);
}

TimerId EventLoop::runAfter(TimerDuration delay, Functor cb) {
    return timerQueue_->addTimer(std::move(cb), mini::base::now() + delay, TimerDuration::zero());
}

TimerId EventLoop::runEvery(TimerDuration interval, Functor cb) {
    if (interval <= TimerDuration::zero()) {
        throw std::invalid_argument("runEvery interval must be positive");
    }
    return timerQueue_->addTimer(std::move(cb), mini::base::now() + interval, interval);
}

void EventLoop::cancel(TimerId timerId) {
    timerQueue_->cancel(timerId);
}

void EventLoop::wakeup() {
    wakeupCount_.fetch_add(1, std::memory_order_relaxed);
    const ssize_t written = writeWakeup(wakeupFds_.writeFd);
    if (written < 0 && !sockets::isWouldBlock(sockets::lastError())) {
        LOG_SYSERR << "EventLoop::wakeup: " << sockets::errorMessage(sockets::lastError());
    }
}

void EventLoop::updateChannel(Channel* channel) {
    assertInLoopThread();
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel) {
    assertInLoopThread();
    poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel* channel) {
    assertInLoopThread();
    return poller_->hasChannel(channel);
}

bool EventLoop::isInLoopThread() const noexcept {
    return threadId_ == std::this_thread::get_id();
}

void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread()) {
        throw std::runtime_error("EventLoop used from a different thread");
    }
}

void EventLoop::handleRead(mini::base::Timestamp receiveTime) {
    (void)receiveTime;
    if (!drainWakeup(wakeupFds_.readFd) && !sockets::isWouldBlock(sockets::lastError())) {
        LOG_SYSERR << "EventLoop::handleRead: " << sockets::errorMessage(sockets::lastError());
    }
    EventLoopMetricSample sample;
    sample.event = EventLoopMetricEvent::WakeupHandled;
    sample.loop = this;
    sample.wakeupCount = wakeupCount_.load(std::memory_order_relaxed);
    emitEventLoopMetric(sample);
}

void EventLoop::doPendingFunctors() {
    std::vector<PendingFunctor> functors;
    std::size_t pendingPeak = 0;
    callingPendingFunctors_ = true;

    {
        std::lock_guard lock(mutex_);
        functors.swap(pendingFunctors_);
        pendingPeak = pendingFunctorPeak_.exchange(0, std::memory_order_relaxed);
    }

    if (!functors.empty()) {
        const auto now = mini::base::now();
        EventLoopMetricSample sample;
        sample.event = EventLoopMetricEvent::PendingFunctorsDrained;
        sample.loop = this;
        sample.pendingFunctors = functors.size();
        sample.pendingFunctorPeak = std::max(pendingPeak, functors.size());
        sample.wakeupCount = wakeupCount_.load(std::memory_order_relaxed);
        sample.oldestPendingLatency = now - functors.front().enqueuedAt;
        emitEventLoopMetric(sample);
    }

    for (auto& functor : functors) {
        functor.functor();
    }

    callingPendingFunctors_ = false;
}

void EventLoop::emitEventLoopMetric(EventLoopMetricSample sample) {
    if (!eventLoopMetricCallback_) {
        return;
    }
    sample.loop = this;
    eventLoopMetricCallback_(sample);
}

}  // namespace mini::net
