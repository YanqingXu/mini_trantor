#pragma once

// TcpClient 是客户端 TCP 连接管理器，与 TcpServer 对称。
// 它通过 Connector 发起连接，管理连接建立后的 TcpConnection，
// 支持可配置的重连策略，所有状态变更在 owner loop 线程执行。

#include "mini/base/noncopyable.h"
#include "mini/net/Callbacks.h"
#include "mini/net/InetAddress.h"

#include <memory>
#include <mutex>
#include <string>

namespace mini::net {

class Connector;
class EventLoop;

class TcpClient : private mini::base::noncopyable {
public:
    TcpClient(EventLoop* loop, const InetAddress& serverAddr, std::string name);
    ~TcpClient();

    /// Connect to the server. Safe to call cross-thread.
    void connect();

    /// Disconnect from the server. Safe to call cross-thread.
    void disconnect();

    /// Stop connector (cancel pending connect/retry). Safe to call cross-thread.
    void stop();

    /// Enable automatic reconnect on disconnect.
    void enableRetry() noexcept;
    void disableRetry() noexcept;
    bool retry() const noexcept;

    const std::string& name() const noexcept;
    EventLoop* getLoop() const noexcept;
    TcpConnectionPtr connection() const;

    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setWriteCompleteCallback(WriteCompleteCallback cb);

private:
    void newConnection(int sockfd);
    void removeConnection(const TcpConnectionPtr& conn);

    EventLoop* loop_;
    std::string name_;
    std::shared_ptr<Connector> connector_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    bool retry_;
    bool connect_;
    int nextConnId_;
    mutable std::mutex mutex_;
    TcpConnectionPtr connection_;  // guarded by mutex_
};

}  // namespace mini::net
