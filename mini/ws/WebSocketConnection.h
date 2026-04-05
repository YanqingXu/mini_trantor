#pragma once

// WebSocketConnection 是升级后的 per-connection WebSocket 状态。
// 存储在 TcpConnection::setContext 中，管理帧解码和消息分发。
// 只在连接的 owner loop 线程上访问。

#include "mini/net/Buffer.h"
#include "mini/net/Callbacks.h"
#include "mini/ws/WebSocketCodec.h"

#include <functional>
#include <string>

namespace mini::ws {

class WebSocketConnection {
public:
    using MessageCallback = std::function<void(const mini::net::TcpConnectionPtr&,
                                               std::string message,
                                               WsOpcode opcode)>;
    using CloseCallback = std::function<void(const mini::net::TcpConnectionPtr&,
                                             WsCloseCode code,
                                             const std::string& reason)>;

    WebSocketConnection() = default;

    void setMessageCallback(MessageCallback cb) { messageCallback_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { closeCallback_ = std::move(cb); }

    /// Process incoming data from the TcpConnection buffer.
    /// Returns false if a protocol error occurred and the connection should be closed.
    bool onData(const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf);

    /// Send a text message.
    static void sendText(const mini::net::TcpConnectionPtr& conn, std::string_view text);

    /// Send a binary message.
    static void sendBinary(const mini::net::TcpConnectionPtr& conn, std::string_view data);

    /// Send a close frame and shutdown.
    static void sendClose(const mini::net::TcpConnectionPtr& conn,
                          WsCloseCode code = WsCloseCode::kNormal,
                          std::string_view reason = {});

    /// Whether a close frame has been sent.
    bool closeSent() const noexcept { return closeSent_; }

private:
    MessageCallback messageCallback_;
    CloseCallback closeCallback_;
    bool closeSent_{false};
    bool closeReceived_{false};
};

}  // namespace mini::ws
