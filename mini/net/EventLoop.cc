#include "mini/net/EventLoop.h"

#include "mini/net/Channel.h"
#include "mini/net/Poller.h"
#include "mini/net/TimerQueue.h"

#include "mini/base/Logger.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <sys/eventfd.h>
#include <unistd.h>

namespace mini::net {

namespace {

thread_local EventLoop* t_loopInThisThread = nullptr;

int createEventfd() {
    const int eventfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventfd < 0) {
        LOG_SYSFATAL << "eventfd: " << std::strerror(errno);
    }
    return eventfd;
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
      wakeupFd_(createEventfd()),
      wakeupChannel_(std::make_unique<Channel>(this, wakeupFd_)),
      currentActiveChannel_(nullptr) {
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
    ::close(wakeupFd_);
    t_loopInThisThread = nullptr;
}

void EventLoop::loop() {
    assertInLoopThread();
    looping_ = true;
    quit_ = false;

    while (!quit_) {
        activeChannels_.clear();
        pollReturnTime_ = poller_->poll(10000, &activeChannels_);
        eventHandling_ = true;
        for (Channel* channel : activeChannels_) {
            currentActiveChannel_ = channel;
            channel->handleEvent(pollReturnTime_);
        }
        currentActiveChannel_ = nullptr;
        eventHandling_ = false;
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
    {
        std::lock_guard lock(mutex_);
        pendingFunctors_.push_back(std::move(cb));
    }

    if (!isInLoopThread() || callingPendingFunctors_) {
        wakeup();
    }
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
    const uint64_t one = 1;
    const ssize_t written = ::write(wakeupFd_, &one, sizeof(one));
    if (written != static_cast<ssize_t>(sizeof(one)) && errno != EAGAIN) {
        LOG_SYSERR << "EventLoop::wakeup: " << std::strerror(errno);
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
    uint64_t one = 0;
    const ssize_t n = ::read(wakeupFd_, &one, sizeof(one));
    if (n != static_cast<ssize_t>(sizeof(one)) && errno != EAGAIN) {
        LOG_SYSERR << "EventLoop::handleRead: " << std::strerror(errno);
    }
}

void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;

    {
        std::lock_guard lock(mutex_);
        functors.swap(pendingFunctors_);
    }

    for (auto& functor : functors) {
        functor();
    }

    callingPendingFunctors_ = false;
}

}  // namespace mini::net
