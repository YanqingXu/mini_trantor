#pragma once

// EventLoopThreadPool 提供 one-loop-per-thread 的扩展模型。
// 它在 base loop 线程中启动 worker loops，并按轮转策略返回下一个 loop。
// 全部初始化成功才发布；停止经每个 worker 的拥有者发出，先请求全部退出再 join。

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

    /// Stopped-only configuration; exclusive access before handoff to the base loop.
    void setThreadNum(int numThreads);
    void start(const ThreadInitCallback& callback = ThreadInitCallback());
    void stop();

    EventLoop* getNextLoop();
    std::vector<EventLoop*> getAllLoops() const;

private:
    enum class State { Stopped, Starting, Running, Stopping };
    static void stopWorkers(std::vector<std::unique_ptr<EventLoopThread>>& threads);
    void assertStable() const;
    EventLoop* baseLoop_;
    std::string name_;
    State state_{State::Stopped};
    int numThreads_;
    int next_;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

}  // namespace mini::net
