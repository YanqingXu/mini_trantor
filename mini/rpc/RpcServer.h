#pragma once

// RpcServer 是 TcpServer 的 RPC 协议适配器。
// 它为每个连接维护 RpcChannel，将原始字节流解码为 RPC 请求，
// 分发到已注册的 method handler，然后通过 RpcChannel 发送响应。
// 遵守 one-loop-per-thread 线程模型。

#include "mini/rpc/RpcChannel.h"
#include "mini/net/Callbacks.h"
#include "mini/net/TcpServer.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mini::net {
class EventLoop;
class InetAddress;
}  // namespace mini::net

namespace mini::rpc {

/// Handler for a single RPC method.
/// @param payload   The request payload.
/// @param respond   Call with response payload to send back.
/// @param respondError  Call with error message to send error back.
using RpcMethodHandler = std::function<void(std::string_view payload,
                                            std::function<void(std::string_view)> respond,
                                            std::function<void(std::string_view)> respondError)>;

class RpcServer {
public:
    RpcServer(mini::net::EventLoop* loop,
              const mini::net::InetAddress& listenAddr,
              std::string name,
              bool reusePort = true);

    /// Register a method handler. Must be called before start().
    void registerMethod(std::string method, RpcMethodHandler handler);

    void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }

    void start();

private:
    void onConnection(const mini::net::TcpConnectionPtr& conn);
    void onMessage(const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf);

    mini::net::TcpServer server_;
    std::unordered_map<std::string, RpcMethodHandler> methods_;
};

}  // namespace mini::rpc
