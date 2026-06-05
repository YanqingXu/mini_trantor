#pragma once

// MetricsHook 定义可观测性的事件枚举和回调类型。
// 所有回调在 owner EventLoop 线程上同步调用，不违反 one-loop-per-thread 纪律。
// 不设回调时零开销（if (callback_) callback_(...) 模式）。
//
// 注意：TcpConnectionPtr 定义在 mini/net/Callbacks.h 中。
// 本头文件仅前向声明，回调签名使用 std::shared_ptr<TcpConnection>。

#include <cstddef>
#include <functional>
#include <memory>

namespace mini::net {

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

}  // namespace mini::net
