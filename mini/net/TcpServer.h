#pragma once

// TcpServer 协调 Acceptor、线程池和连接生命周期。
// 连接映射由 base loop 线程维护，跨 loop 移除必须显式回流。
// 可选支持 TLS：通过 enableSsl() 配置后，新连接自动执行 TLS 握手。
// v5-delta: 支持 TcpServerOptions、ConnectionEvent/BackpressureEvent/TlsEvent hook、
//           drain-aware stop(Duration)。

#include "mini/base/MetricsHook.h"
#include "mini/base/Timestamp.h"
#include "mini/base/noncopyable.h"
#include "mini/net/Acceptor.h"
#include "mini/net/Callbacks.h"
#include "mini/net/buffer/PayloadPool.h"
#include "mini/net/broadcast/BroadcastRouter.h"
#include "mini/net/broadcast/BroadcastDispatcher.h"
#include "mini/net/EventLoopThreadPool.h"
#include "mini/net/SocketTypes.h"
#include "mini/net/TcpServerOptions.h"
#include "mini/net/TimerId.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mini::net {

class EventLoop;
class TlsContext;

class TcpServer : private mini::base::noncopyable {
public:
    using Duration = std::chrono::steady_clock::duration;
    using BroadcastAdmissionCallback = std::function<bool(const BroadcastMetricSample&)>;

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
    void setLogicMessageCallback(LogicMessageCallback cb);
    void setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark);
    void setWriteCompleteCallback(WriteCompleteCallback cb);
    std::size_t connectionCount() const;
    void bindBroadcastSession(const TcpConnectionPtr& connection, std::string sessionId);
    void unbindBroadcastSession(std::string sessionId);
    void unbindBroadcastSession(const TcpConnectionPtr& connection, std::string sessionId);
    void joinBroadcastGroup(std::string sessionId, std::string groupId);
    void leaveBroadcastGroup(std::string sessionId, std::string groupId);
    void joinBroadcastAoi(std::string sessionId, std::string aoiId);
    void leaveBroadcastAoi(std::string sessionId, std::string aoiId);
    void broadcastTo(const std::vector<std::string>& sessionIds,
                     const std::string& data,
                     std::uint32_t priority = 1);
    void broadcastGroup(std::string groupId, const std::string& data, std::uint32_t priority = 1);
    void broadcastAoi(std::string aoiId, const std::string& data, std::uint32_t priority = 1);
    void broadcast(const std::string& data, std::uint32_t priority = 1);
    void broadcastToInLoop(std::vector<std::string> sessionIds, buffer::PayloadPtr payload);
    void broadcastInLoop(buffer::PayloadPtr payload);

    // ── Metrics hooks (v5-delta) ──

    /// Set connection lifecycle event hook. Callback fires on owner loop thread.
    void setConnectionEventCallback(ConnectionEventCallback cb);

    /// Set backpressure event hook. Callback fires on owner loop thread.
    void setBackpressureEventCallback(BackpressureEventCallback cb);

    /// Set TLS handshake event hook. Callback fires on owner loop thread.
    void setTlsEventCallback(TlsEventCallback cb);

    /// Set broadcast fanout / latency metric hook. Callback fires on base or target ioLoop thread.
    void setBroadcastMetricCallback(BroadcastMetricCallback cb);

    /// Set broadcast admission hook. Callback fires on base loop after routing and before dispatch.
    void setBroadcastAdmissionCallback(BroadcastAdmissionCallback cb);

    /// Set EventLoop queue / wakeup metric hook for base and worker loops.
    void setEventLoopMetricCallback(EventLoopMetricCallback cb);

    // ── Lifecycle ──

    void start();

    /// Force-close all connections and stop. Idempotent.
    void stop();

    /// Drain-aware stop: wait up to drainTimeout for connections to close,
    /// then force-close remaining. Must be called on base loop thread.
    void stop(Duration drainTimeout);

private:
    void newConnection(SocketFd sockfd, const InetAddress& peerAddr);
    void removeConnection(const TcpConnectionPtr& connection);
    void removeConnectionInLoop(const TcpConnectionPtr& connection);
    void broadcastToInLoopWithMetrics(
        std::vector<std::string> sessionIds,
        buffer::PayloadPtr payload,
        mini::base::Timestamp requestedAt,
        std::uint32_t priority = 1);
    void broadcastBucketInLoopWithMetrics(
        std::vector<broadcast::BroadcastRouter::LoopBatch> batches,
        buffer::PayloadPtr payload,
        mini::base::Timestamp requestedAt,
        bool targeted,
        std::size_t requestedSessions,
        std::uint32_t priority = 1);
    void broadcastInLoopWithMetrics(buffer::PayloadPtr payload,
                                    mini::base::Timestamp requestedAt,
                                    std::uint32_t priority = 1);
    void configureBroadcastMetrics();
    bool admitBroadcast(const broadcast::BroadcastDispatcher::DispatchMetricContext& metrics) const;
    void forceCloseAllConnections();
    void onDrainTimeout();

    EventLoop* loop_;
    const std::string name_;
    std::unique_ptr<Acceptor> acceptor_;
    std::shared_ptr<EventLoopThreadPool> threadPool_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    LogicMessageCallback logicMessageCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    ThreadInitCallback threadInitCallback_;

    // Metrics hooks (v5-delta)
    ConnectionEventCallback connectionEventCallback_;
    BackpressureEventCallback backpressureEventCallback_;
    TlsEventCallback tlsEventCallback_;
    BroadcastMetricCallback broadcastMetricCallback_;
    BroadcastAdmissionCallback broadcastAdmissionCallback_;
    EventLoopMetricCallback eventLoopMetricCallback_;

    std::atomic<bool> started_;
    bool stopped_;
    bool draining_;
    bool broadcastMetricsEnabled_;
    bool eventLoopQueueMetricsEnabled_;
    int nextConnId_;
    std::size_t highWaterMark_;
    std::size_t backpressureHighWaterMark_;
    std::size_t backpressureLowWaterMark_;
    Duration idleTimeout_;
    std::unordered_map<std::string, TcpConnectionPtr> connections_;
    std::shared_ptr<broadcast::BroadcastRouter> broadcastRouter_;
    std::shared_ptr<broadcast::BroadcastDispatcher> broadcastDispatcher_;
    std::shared_ptr<buffer::PayloadPool> payloadPool_;
    std::shared_ptr<void> lifetimeToken_;
    std::shared_ptr<TlsContext> tlsContext_;
    TimerId drainTimerId_;
};

}  // namespace mini::net
