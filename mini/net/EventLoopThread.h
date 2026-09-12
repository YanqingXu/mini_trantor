#pragma once

// EventLoopThread 管理一个后台线程中的单个 EventLoop 生命周期。
// 启动结果由状态发布；提前退出后保留 loop 到 stop/restart/析构边界。
// 外部控制串行化；worker 自停只请求退出，由外部拥有者完成 join。

#include "mini/base/noncopyable.h"
#include "mini/net/Callbacks.h"

#include <condition_variable>
#include <exception>
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
    void stop();

private:
    enum class State { Idle, Starting, Running, Exited, Stopping, Stopped, Failed };
    void threadFunc();
    void requestStop();
    void requestStopLocked();
    bool isManagedThread();
    bool running();

    EventLoop* loop_{nullptr};
    std::jthread thread_;
    std::mutex controlMutex_;
    std::mutex mutex_;
    std::condition_variable condition_;
    State state_{State::Idle};
    std::thread::id workerId_;
    std::exception_ptr failure_;
    ThreadInitCallback callback_;
    std::string name_;
    friend class EventLoopThreadPool;
};

}  // namespace mini::net
