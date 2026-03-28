#pragma once

#include "mini/base/noncopyable.h"
#include "mini/net/Callbacks.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace mini::net {

class EventLoop;

class EventLoopThread : private mini::base::noncopyable {
public:
    EventLoopThread(ThreadInitCallback callback = {}, std::string name = {});
    ~EventLoopThread();

    EventLoop* startLoop();

private:
    void threadFunc();

    EventLoop* loop_;
    bool exiting_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    ThreadInitCallback callback_;
    std::string name_;
};

}  // namespace mini::net
