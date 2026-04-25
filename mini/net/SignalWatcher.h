#pragma once

// SignalWatcher 通过 signalfd 将 POSIX 信号接入 EventLoop。
// 它将 SIGINT/SIGTERM 等信号转换为 Channel 可读事件，
// 使得信号处理在 owner loop 线程上安全执行，无需异步信号安全约束。
//
// SignalWatcher 还在构造时全局屏蔽 SIGPIPE（IGN），
// 防止写入已关闭连接时触发默认的进程终止行为。

#include "mini/base/noncopyable.h"
#include "mini/net/Channel.h"

#include <functional>
#include <signal.h>
#include <vector>

namespace mini::net {

class EventLoop;

class SignalWatcher : private mini::base::noncopyable {
public:
    using SignalCallback = std::function<void(int signum)>;

    /// Block the given signals in the calling thread's signal mask.
    /// Call this on the main thread BEFORE spawning any worker threads
    /// so that all subsequently created threads inherit the blocked mask.
    /// This ensures that signals are delivered to signalfd rather than
    /// via default handlers that would terminate the process.
    static void blockSignals(const std::vector<int>& signals = {SIGINT, SIGTERM});

    /// Ignore SIGPIPE process-wide.  Safe to call repeatedly.
    static void ignoreSigpipe();

    /// Construct a SignalWatcher that monitors the given signals on the
    /// specified EventLoop.  SIGPIPE is ignored process-wide as a side
    /// effect (once is sufficient; repeated calls are harmless).
    /// The monitored signals are also blocked in the calling thread.
    /// IMPORTANT: For multi-threaded programs, call blockSignals() on
    /// the main thread before creating worker threads to ensure all
    /// threads have the signals blocked.
    explicit SignalWatcher(EventLoop* loop, std::vector<int> signals = {SIGINT, SIGTERM});

    ~SignalWatcher();

    /// Install a callback to be invoked on the owner loop thread when a
    /// monitored signal is received.  Replaces any previously set callback.
    void setSignalCallback(SignalCallback cb);

    /// Returns the list of signals being monitored.
    const std::vector<int>& signals() const noexcept;

private:
    void handleRead();

    EventLoop* loop_;
    std::vector<int> signals_;
    int signalfd_;
    std::unique_ptr<Channel> signalChannel_;
    SignalCallback callback_;
};

}  // namespace mini::net
