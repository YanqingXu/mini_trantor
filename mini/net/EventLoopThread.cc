#include "mini/net/EventLoopThread.h"

#include "mini/net/EventLoop.h"

namespace mini::net {

EventLoopThread::EventLoopThread(ThreadInitCallback callback, std::string name)
    : loop_(nullptr), callback_(std::move(callback)), name_(std::move(name)) {
}

EventLoopThread::~EventLoopThread() {
    stop();
}

EventLoop* EventLoopThread::startLoop() {
    {
        std::lock_guard lock(mutex_);
        if (loop_ != nullptr) {
            return loop_;
        }
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    thread_ = std::jthread([this] { threadFunc(); });

    EventLoop* loop = nullptr;
    {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return loop_ != nullptr; });
        loop = loop_;
    }
    return loop;
}

void EventLoopThread::stop() {
    {
        std::lock_guard lock(mutex_);
        if (loop_ != nullptr) {
            loop_->quit();
        }
    }

    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
}

void EventLoopThread::threadFunc() {
    EventLoop loop;
    if (callback_) {
        callback_(&loop);
    }

    {
        std::lock_guard lock(mutex_);
        loop_ = &loop;
        condition_.notify_one();
    }

    loop.loop();

    {
        std::lock_guard lock(mutex_);
        loop_ = nullptr;
    }
}

}  // namespace mini::net
