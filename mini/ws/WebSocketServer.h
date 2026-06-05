#pragma once

// WebSocketServer 是基于 TcpServer 的 WebSocket 服务器。
// 它先以 HTTP 模式接收请求，检测到 Upgrade: websocket 后执行握手，
// 将连接切换到 WebSocket 帧模式。
// 遵守 one-loop-per-thread 线程模型。

#include "mini/net/Callbacks.h"
#include "mini/net/TcpServer.h"
#include "mini/ws/WebSocketCodec.h"
#include "mini/ws/WebSocketConnection.h"

#include <functional>
#include <string>

namespace mini::net {
class EventLoop;
class InetAddress;
}  // namespace mini::net

namespace mini::ws {

using WsMessageCallback = std::function<void(const mini::net::TcpConnectionPtr&,
                                             std::string message,
                                             WsOpcode opcode)>;
using WsConnectCallback = std::function<void(const mini::net::TcpConnectionPtr&)>;
using WsCloseCallback = std::function<void(const mini::net::TcpConnectionPtr&,
                                           WsCloseCode code,
                                           const std::string& reason)>;

class WebSocketServer {
public:
    WebSocketServer(mini::net::EventLoop* loop,
                    const mini::net::InetAddress& listenAddr,
                    std::string name,
                    bool reusePort = true);

    void setMessageCallback(WsMessageCallback cb) { messageCallback_ = std::move(cb); }
    void setConnectCallback(WsConnectCallback cb) { connectCallback_ = std::move(cb); }
    void setCloseCallback(WsCloseCallback cb) { closeCallback_ = std::move(cb); }
    void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }

    void start();

private:
    /// Holds per-connection state: either HTTP parsing or WebSocket mode.
    struct ConnectionContext {
        bool upgraded{false};
        WebSocketConnection wsConn;
    };

    void onConnection(const mini::net::TcpConnectionPtr& conn);
    void onMessage(const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf);
    bool tryUpgrade(const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf,
                    ConnectionContext& ctx);

    mini::net::TcpServer server_;
    WsMessageCallback messageCallback_;
    WsConnectCallback connectCallback_;
    WsCloseCallback closeCallback_;
};

}  // namespace mini::ws
