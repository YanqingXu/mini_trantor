#pragma once

// EventLoopThreadPool 提供 one-loop-per-thread 的扩展模型。
// 它在 base loop 线程中启动 worker loops，并按轮转策略返回下一个 loop。

#include "mini/base/noncopyable.h"
#include "mini/net/Callbacks.h"

#include <memory>
#include <string>
#include <vector>

namespace mini::net {

class EventLoop;
class EventLoopThread;

class EventLoopThreadPool : private mini::base::noncopyable {
public:
    EventLoopThreadPool(EventLoop* baseLoop, std::string name);
    ~EventLoopThreadPool();

    void setThreadNum(int numThreads);
    void start(const ThreadInitCallback& callback = ThreadInitCallback());

    EventLoop* getNextLoop();
    std::vector<EventLoop*> getAllLoops() const;

private:
    EventLoop* baseLoop_;
    std::string name_;
    bool started_;
    int numThreads_;
    int next_;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

}  // namespace mini::net
