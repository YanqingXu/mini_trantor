#include "mini/net/EventLoopThread.h"

#include "mini/net/EventLoop.h"
#include "mini/base/Logger.h"

#include <stdexcept>

namespace mini::net {

EventLoopThread::EventLoopThread(ThreadInitCallback callback, std::string name)
    : callback_(std::move(callback)), name_(std::move(name)) {
}

EventLoopThread::~EventLoopThread() {
    if (isManagedThread()) {
        LOG_FATAL << "EventLoopThread destroyed from its managed thread";
    }
    stop();
}

EventLoop* EventLoopThread::startLoop() {
    // A callback must not wait on the external starter holding controlMutex_.
    if (isManagedThread()) {
        std::lock_guard lock(mutex_);
        if (state_ == State::Running) { return loop_; }
        throw std::logic_error("startLoop cannot re-enter during startup or stop");
    }
    std::lock_guard control(controlMutex_);
    {
        std::lock_guard lock(mutex_);
        if (state_ == State::Running) { return loop_; }
    }
    requestStop(); // release a naturally exited worker's retained stack object
    if (thread_.joinable()) {
        thread_.join();
    }
    {
        std::lock_guard lock(mutex_);
        state_ = State::Starting;
        failure_ = {};
    }
    try {
        thread_ = std::jthread([this] { threadFunc(); });
    } catch (...) {
        std::lock_guard lock(mutex_);
        state_ = State::Failed;
        failure_ = std::current_exception();
        throw;
    }

    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] {
        return state_ == State::Running || state_ == State::Exited ||
               state_ == State::Stopped || state_ == State::Failed;
    });
    if (state_ == State::Running || state_ == State::Exited) { return loop_; }
    auto failure = failure_;
    lock.unlock();
    thread_.join();
    if (failure) { std::rethrow_exception(failure); }
    throw std::runtime_error("EventLoopThread stopped before startup completed");
}

void EventLoopThread::stop() {
    if (isManagedThread()) {
        requestStop();
        return;
    }
    std::lock_guard control(controlMutex_);
    requestStop();
    if (thread_.joinable()) { thread_.join(); }
}

bool EventLoopThread::isManagedThread() {
    std::lock_guard lock(mutex_);
    return workerId_ == std::this_thread::get_id();
}

bool EventLoopThread::running() {
    std::lock_guard lock(mutex_);
    return state_ == State::Running && !loop_->quit_.load();
}

void EventLoopThread::requestStopLocked() {
    if (state_ == State::Starting || state_ == State::Running || state_ == State::Exited) {
        state_ = State::Stopping;
    }
    // Hold the state lock THROUGH wakeup. Worker unpublication precedes fd close.
    if (loop_) { loop_->quit(); }
    condition_.notify_all();
}

void EventLoopThread::requestStop() {
    std::lock_guard lock(mutex_);
    requestStopLocked();
}

void EventLoopThread::threadFunc() {
    std::exception_ptr failure;
    try {
        EventLoop loop;
        {
            std::lock_guard lock(mutex_);
            workerId_ = std::this_thread::get_id();
            loop_ = &loop; // private during Starting; not yet a published result
        }
        try {
            if (callback_) { callback_(&loop); }
            if (loop.quit_.load()) {
                throw std::runtime_error("EventLoopThread initializer requested quit");
            }
        } catch (...) {
            failure = std::current_exception();
        }
        if (!failure) {
            {
                std::lock_guard lock(mutex_);
                state_ = State::Running;
                condition_.notify_all();
            }
            loop.loop();
            std::unique_lock lock(mutex_);
            if (state_ != State::Stopping) {
                state_ = State::Exited;
                condition_.notify_all();
                condition_.wait(lock, [this] { return state_ == State::Stopping; });
            }
        }
        {
            std::lock_guard lock(mutex_);
            loop_ = nullptr;
        }
        // The stack EventLoop is destroyed here, on its owner, after unpublication.
    } catch (...) {
        failure = std::current_exception(); // construction failure before publication
    }
    {
        std::lock_guard lock(mutex_);
        loop_ = nullptr;
        workerId_ = {};
        failure_ = failure;
        state_ = failure ? State::Failed : State::Stopped;
        condition_.notify_all();
    }
}

}  // namespace mini::net
