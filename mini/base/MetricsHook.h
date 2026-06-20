#pragma once

// MetricsHook 定义可观测性的事件枚举和回调类型。
// 所有回调在 owner EventLoop 线程上同步调用，不违反 one-loop-per-thread 纪律。
// 不设回调时零开销（if (callback_) callback_(...) 模式）。
//
// 注意：TcpConnectionPtr 定义在 mini/net/Callbacks.h 中。
// 本头文件仅前向声明，回调签名使用 std::shared_ptr<TcpConnection>。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mini::net {

class EventLoop;
class TcpConnection;
class InetAddress;

// ── 连接生命周期事件 ──

enum class ConnectionEvent {
    Connected,       ///< 连接已建立（TLS 握手完成后）
    Disconnected,    ///< 连接已断开（正常关闭或错误）
    IdleTimeout,     ///< 空闲超时触发关闭
    ForceClosed,     ///< 被强制关闭（forceClose / server stop）
};

using ConnectionEventCallback =
    std::function<void(const std::shared_ptr<TcpConnection>&, ConnectionEvent)>;

// ── 背压控制事件 ──

enum class BackpressureEvent {
    ReadPaused,   ///< 输出缓冲区 >= 高水位，暂停读取
    ReadResumed,  ///< 输出缓冲区 <= 低水位，恢复读取
};

using BackpressureEventCallback =
    std::function<void(const std::shared_ptr<TcpConnection>&, BackpressureEvent, std::size_t bufferedBytes)>;

// ── 连接器事件 ──

enum class ConnectorEvent {
    ConnectAttempt,       ///< 发起连接尝试
    ConnectSuccess,       ///< 连接成功
    ConnectFailed,        ///< 连接失败（被拒绝/网络不可达等）
    RetryScheduled,       ///< 重试已排定
    SelfConnectDetected,  ///< 检测到自连接
    ConnectTimeout,       ///< 连接超时
};

using ConnectorEventCallback = std::function<void(const InetAddress&, ConnectorEvent)>;

// ── TLS 握手事件 ──

enum class TlsEvent {
    HandshakeStarted,    ///< TLS 握手开始
    HandshakeCompleted,  ///< TLS 握手完成
    HandshakeFailed,     ///< TLS 握手失败
};

using TlsEventCallback =
    std::function<void(const std::shared_ptr<TcpConnection>&, TlsEvent)>;

// ── 广播分发指标 ──

enum class BroadcastMetricEvent {
    Routed,       ///< base loop 已完成目标分桶
    LoopFlushed,  ///< owner ioLoop 已执行本 loop 的批量发送
};

struct BroadcastMetricSample {
    using Duration = std::chrono::steady_clock::duration;

    BroadcastMetricEvent event{BroadcastMetricEvent::Routed};
    EventLoop* loop{nullptr};
    bool targeted{false};
    std::size_t requestedSessions{0};
    std::size_t loopBatches{0};
    std::size_t fanoutConnections{0};
    std::size_t payloadBytes{0};
    Duration routeLatency{Duration::zero()};
    Duration queueLatency{Duration::zero()};
    Duration fanoutLatency{Duration::zero()};
};

using BroadcastMetricCallback = std::function<void(const BroadcastMetricSample&)>;

// ── EventLoop 队列 / wakeup 指标 ──

enum class EventLoopMetricEvent {
    PendingFunctorsDrained,  ///< owner loop 开始执行一批 pending functors
    WakeupHandled,           ///< owner loop 消费了一次 wakeup 信号
};

struct EventLoopMetricSample {
    using Duration = std::chrono::steady_clock::duration;

    EventLoopMetricEvent event{EventLoopMetricEvent::PendingFunctorsDrained};
    EventLoop* loop{nullptr};
    std::size_t pendingFunctors{0};
    std::size_t pendingFunctorPeak{0};
    std::uint64_t wakeupCount{0};
    Duration oldestPendingLatency{Duration::zero()};
};

using EventLoopMetricCallback = std::function<void(const EventLoopMetricSample&)>;

}  // namespace mini::net

namespace mini::game {

// ── 会话重连指标 ──

enum class SessionMetricEvent {
    ReconnectWindowStarted,  ///< session 进入可重连窗口
    ReconnectSucceeded,      ///< session 在窗口内重绑 transport
    ReconnectExpired,        ///< session 重连窗口超时
};

struct SessionMetricSample {
    using Duration = std::chrono::steady_clock::duration;

    SessionMetricEvent event{SessionMetricEvent::ReconnectWindowStarted};
    std::string sessionToken;
    bool success{false};
    Duration reconnectDuration{Duration::zero()};
};

using SessionMetricCallback = std::function<void(const SessionMetricSample&)>;

namespace logic {

// ── 固定步逻辑队列指标 ──

enum class LogicLoopMetricEvent {
    CommandEnqueued,  ///< 命令进入逻辑队列后，在 logic loop 上观测
    TickCompleted,    ///< 一个 fixed-step tick 完成
};

struct LogicLoopMetricSample {
    using Duration = std::chrono::steady_clock::duration;

    LogicLoopMetricEvent event{LogicLoopMetricEvent::CommandEnqueued};
    mini::net::EventLoop* loop{nullptr};
    std::size_t backlog{0};
    std::size_t drainedCommands{0};
    std::chrono::milliseconds oldestLag{std::chrono::milliseconds::zero()};
    Duration tickDuration{Duration::zero()};
    Duration tickJitter{Duration::zero()};
};

using LogicLoopMetricCallback = std::function<void(const LogicLoopMetricSample&)>;

}  // namespace logic

}  // namespace mini::game
