#include "mini/net/EventLoopThreadPool.h"

#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/base/Logger.h"

#include <stdexcept>

namespace mini::net {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, std::string name)
    : baseLoop_(baseLoop), name_(std::move(name)), numThreads_(0), next_(0) {
    if (!baseLoop_) { throw std::invalid_argument("thread pool requires a base loop"); }
}

EventLoopThreadPool::~EventLoopThreadPool() {
    for (auto& thread : threads_) {
        if (thread->isManagedThread()) {
            LOG_FATAL << "EventLoopThreadPool destroyed from one of its workers";
        }
    }
    state_ = State::Stopping;
    stopWorkers(threads_);
}

void EventLoopThreadPool::setThreadNum(int numThreads) {
    // Configuration may precede publication to the base loop (e.g. TcpServer).
    // The caller owns exclusive access until the synchronized handoff.
    if (numThreads < 0) { throw std::invalid_argument("worker count must be non-negative"); }
    if (state_ != State::Stopped) {
        throw std::logic_error("worker count can only change while stopped");
    }
    numThreads_ = numThreads;
}

void EventLoopThreadPool::start(const ThreadInitCallback& callback) {
    baseLoop_->assertInLoopThread();
    assertStable();
    if (state_ == State::Running) { return; }
    state_ = State::Starting;
    std::vector<std::unique_ptr<EventLoopThread>> threads;
    std::vector<EventLoop*> loops;
    try {
        threads.reserve(static_cast<std::size_t>(numThreads_));
        loops.reserve(static_cast<std::size_t>(numThreads_));
        for (int i = 0; i < numThreads_; ++i) {
            auto thread = std::make_unique<EventLoopThread>(callback, name_ + std::to_string(i));
            loops.push_back(thread->startLoop());
            threads.push_back(std::move(thread));
        }
        if (numThreads_ == 0 && callback) { callback(baseLoop_); }
    } catch (...) {
        state_ = State::Stopping;
        stopWorkers(threads);
        state_ = State::Stopped;
        throw;
    }
    threads_ = std::move(threads);
    loops_ = std::move(loops);
    next_ = 0;
    state_ = State::Running;
}

void EventLoopThreadPool::stopWorkers(std::vector<std::unique_ptr<EventLoopThread>>& threads) {
    for (auto& thread : threads) { thread->requestStop(); }
    for (auto& thread : threads) { thread->stop(); }
    threads.clear();
}

void EventLoopThreadPool::stop() {
    baseLoop_->assertInLoopThread();
    assertStable();
    state_ = State::Stopping;
    stopWorkers(threads_);
    loops_.clear();
    state_ = State::Stopped;
    next_ = 0;
}

EventLoop* EventLoopThreadPool::getNextLoop() {
    baseLoop_->assertInLoopThread();
    assertStable();
    EventLoop* loop = baseLoop_;
    if (!loops_.empty()) {
        if (!threads_[static_cast<std::size_t>(next_)]->running()) {
            throw std::runtime_error("selected worker has stopped dispatching");
        }
        loop = loops_[static_cast<std::size_t>(next_)];
        ++next_;
        if (next_ >= static_cast<int>(loops_.size())) {
            next_ = 0;
        }
    }
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops() const {
    baseLoop_->assertInLoopThread();
    assertStable();
    if (loops_.empty()) {
        return {baseLoop_};
    }
    return loops_;
}

void EventLoopThreadPool::assertStable() const {
    if (state_ == State::Starting || state_ == State::Stopping) {
        throw std::logic_error("thread pool control/selection cannot re-enter a transition");
    }
}

}  // namespace mini::net
