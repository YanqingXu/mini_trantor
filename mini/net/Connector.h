#pragma once

// Connector 是 TcpClient 的主动连接适配器，与 Acceptor 对称。
// 它负责发起非阻塞 connect、处理 EINPROGRESS、检测连接就绪，
// 并将已连接的 fd 通过回调交付给上层。所有 Channel 操作在 owner loop 线程。
// start/restart 始终排队；generation 隔离旧事件，终态清理完成后才通知 hook。

#include "mini/base/MetricsHook.h"
#include "mini/base/noncopyable.h"
#include "mini/net/ConnectorOptions.h"
#include "mini/net/InetAddress.h"
#include "mini/net/SocketTypes.h"
#include "mini/net/TimerId.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace mini::net {

class Channel;
class EventLoop;
class Socket;

class Connector : public std::enable_shared_from_this<Connector>, private mini::base::noncopyable {
public:
    using NewConnectionCallback = std::function<void(SocketFd sockfd)>;
    using Duration = std::chrono::steady_clock::duration;

    enum StateE { kDisconnected, kConnecting, kConnected };

    Connector(EventLoop* loop, const InetAddress& serverAddr);
    Connector(EventLoop* loop, const InetAddress& serverAddr, ConnectorOptions options);
    ~Connector();

    void setNewConnectionCallback(NewConnectionCallback cb);

    /// 设置 ConnectorEvent hook。启动前配置或在 owner loop 上替换。
    void setConnectorEventCallback(ConnectorEventCallback cb);

    const InetAddress& serverAddress() const noexcept;
    StateE state() const noexcept;

    /// Queue a connect attempt. Owner-loop only; repeated pending/connected
    /// start is idempotent. ConnectAttempt is never called on this stack.
    void start();

    /// Stop connecting or cancel pending retry. Owner-loop-thread only.
    void stop();

    /// Cancel old work and queue a fresh attempt (restore configured backoff).
    /// Owner-loop only; safe from event/new-connection callbacks.
    void restart();

    /// Configure retry backoff parameters. Must be set before start().
    void setRetryDelay(Duration initial, Duration max);

private:
    enum class Phase { Idle, StartQueued, Connecting, Connected, RetryWaiting };
    void startInLoop(std::uint64_t generation);
    void connecting(Socket& socket, std::uint64_t generation);
    void handleWrite(std::uint64_t generation);
    void handleError(std::uint64_t generation);
    void handleConnectTimeout(std::uint64_t generation);
    void fail(SocketFd sockfd, ConnectorEvent event, std::uint64_t generation,
              bool retryable = true);
    void scheduleRetry(std::uint64_t generation);
    void cancelTimers();
    void emitEvent(ConnectorEvent event);
    SocketFd removeAndResetChannel();

    EventLoop* loop_;
    InetAddress serverAddr_;
    Phase phase_{Phase::Idle};
    std::uint64_t generation_{0};
    bool retryEnabled_;
    NewConnectionCallback newConnectionCallback_;
    ConnectorEventCallback connectorEventCallback_;
    std::unique_ptr<Channel> channel_;
    Duration initialRetryDelay_;
    Duration retryDelayMs_;
    Duration maxRetryDelayMs_;
    Duration connectTimeout_;
    TimerId retryTimerId_;
    TimerId connectTimeoutTimerId_;
};

}  // namespace mini::net
