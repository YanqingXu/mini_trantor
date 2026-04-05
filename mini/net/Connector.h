#pragma once

// Connector 是 TcpClient 的主动连接适配器，与 Acceptor 对称。
// 它负责发起非阻塞 connect、处理 EINPROGRESS、检测连接就绪，
// 并将已连接的 fd 通过回调交付给上层。所有 Channel 操作在 owner loop 线程。

#include "mini/base/noncopyable.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TimerId.h"

#include <chrono>
#include <functional>
#include <memory>

namespace mini::net {

class Channel;
class EventLoop;

class Connector : public std::enable_shared_from_this<Connector>, private mini::base::noncopyable {
public:
    using NewConnectionCallback = std::function<void(int sockfd)>;
    using Duration = std::chrono::steady_clock::duration;

    enum StateE { kDisconnected, kConnecting, kConnected };

    Connector(EventLoop* loop, const InetAddress& serverAddr);
    ~Connector();

    void setNewConnectionCallback(NewConnectionCallback cb);

    const InetAddress& serverAddress() const noexcept;
    StateE state() const noexcept;

    /// Start connecting. Must be called via runInLoop from TcpClient.
    void start();

    /// Stop connecting or cancel pending retry. Owner-loop-thread only.
    void stop();

    /// Restart connecting (reset backoff). Owner-loop-thread only.
    void restart();

    /// Configure retry backoff parameters. Must be set before start().
    void setRetryDelay(Duration initial, Duration max);

private:
    void startInLoop();
    void stopInLoop();
    void connect();
    void connecting(int sockfd);
    void handleWrite();
    void handleError();
    void retry(int sockfd);
    int removeAndResetChannel();
    void resetChannel();

    EventLoop* loop_;
    InetAddress serverAddr_;
    StateE state_;
    bool connect_;
    NewConnectionCallback newConnectionCallback_;
    std::unique_ptr<Channel> channel_;
    Duration retryDelayMs_;
    Duration maxRetryDelayMs_;
    TimerId retryTimerId_;

    static constexpr Duration kDefaultInitRetryDelay = std::chrono::milliseconds(500);
    static constexpr Duration kDefaultMaxRetryDelay = std::chrono::seconds(30);
};

}  // namespace mini::net
