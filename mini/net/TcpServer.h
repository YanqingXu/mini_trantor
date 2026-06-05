#pragma once

// TcpServer 协调 Acceptor、线程池和连接生命周期。
// 连接映射由 base loop 线程维护，跨 loop 移除必须显式回流。
// 可选支持 TLS：通过 enableSsl() 配置后，新连接自动执行 TLS 握手。
// v5-delta: 支持 TcpServerOptions、ConnectionEvent/BackpressureEvent/TlsEvent hook、
//           drain-aware stop(Duration)。

#include "mini/base/MetricsHook.h"
#include "mini/base/noncopyable.h"
#include "mini/net/Acceptor.h"
#include "mini/net/Callbacks.h"
#include "mini/net/EventLoopThreadPool.h"
#include "mini/net/TcpServerOptions.h"
#include "mini/net/TimerId.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

namespace mini::net {

class EventLoop;
class TlsContext;

class TcpServer : private mini::base::noncopyable {
public:
    using Duration = std::chrono::steady_clock::duration;

    TcpServer(EventLoop* loop, const InetAddress& listenAddr, std::string name, bool reusePort = true);
    TcpServer(EventLoop* loop, const InetAddress& listenAddr, std::string name, TcpServerOptions options);
    ~TcpServer();

    void setThreadNum(int numThreads);
    void setIdleTimeout(Duration timeout);
    void setBackpressurePolicy(std::size_t highWaterMark, std::size_t lowWaterMark);
    void setThreadInitCallback(ThreadInitCallback cb);

    /// Enable TLS for all new connections. Must be called before start().
    void enableSsl(std::shared_ptr<TlsContext> tlsContext);
    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark);
    void setWriteCompleteCallback(WriteCompleteCallback cb);
    std::size_t connectionCount() const;

    // ── Metrics hooks (v5-delta) ──

    /// Set connection lifecycle event hook. Callback fires on owner loop thread.
    void setConnectionEventCallback(ConnectionEventCallback cb);

    /// Set backpressure event hook. Callback fires on owner loop thread.
    void setBackpressureEventCallback(BackpressureEventCallback cb);

    /// Set TLS handshake event hook. Callback fires on owner loop thread.
    void setTlsEventCallback(TlsEventCallback cb);

    // ── Lifecycle ──

    void start();

    /// Force-close all connections and stop. Idempotent.
    void stop();

    /// Drain-aware stop: wait up to drainTimeout for connections to close,
    /// then force-close remaining. Must be called on base loop thread.
    void stop(Duration drainTimeout);

private:
    void newConnection(int sockfd, const InetAddress& peerAddr);
    void removeConnection(const TcpConnectionPtr& connection);
    void removeConnectionInLoop(const TcpConnectionPtr& connection);
    void forceCloseAllConnections();
    void onDrainTimeout();

    EventLoop* loop_;
    const std::string name_;
    std::unique_ptr<Acceptor> acceptor_;
    std::shared_ptr<EventLoopThreadPool> threadPool_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    ThreadInitCallback threadInitCallback_;

    // Metrics hooks (v5-delta)
    ConnectionEventCallback connectionEventCallback_;
    BackpressureEventCallback backpressureEventCallback_;
    TlsEventCallback tlsEventCallback_;

    std::atomic<bool> started_;
    bool stopped_;
    bool draining_;
    int nextConnId_;
    std::size_t highWaterMark_;
    std::size_t backpressureHighWaterMark_;
    std::size_t backpressureLowWaterMark_;
    Duration idleTimeout_;
    std::unordered_map<std::string, TcpConnectionPtr> connections_;
    std::shared_ptr<void> lifetimeToken_;
    std::shared_ptr<TlsContext> tlsContext_;
    TimerId drainTimerId_;
};

}  // namespace mini::net
