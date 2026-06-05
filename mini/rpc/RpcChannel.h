#pragma once

// RpcChannel 是 per-connection 的 RPC 请求/响应关联状态。
// 它维护 pending map（requestId → callback）、分配 requestId、管理超时。
// 只在连接的 owner loop 线程上访问。

#include "mini/rpc/RpcCodec.h"
#include "mini/net/Callbacks.h"
#include "mini/net/TimerId.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mini::net {
class EventLoop;
class TcpConnection;
class Buffer;
}  // namespace mini::net

namespace mini::rpc {

/// Callback invoked when an RPC response or error arrives (or timeout).
/// @param errorMsg  Empty string on success; non-empty on error/timeout.
/// @param payload   Response payload (empty on error/timeout).
using RpcResponseCallback = std::function<void(const std::string& errorMsg,
                                               const std::string& payload)>;

/// Callback invoked when an RPC request arrives on the server side.
/// @param method   The service method name.
/// @param payload  The request payload.
/// @param respond  Function to call with (payload) to send response, or (errorMsg) for error.
using RpcRequestCallback = std::function<void(std::string_view method,
                                              std::string_view payload,
                                              std::function<void(std::string_view)> respond,
                                              std::function<void(std::string_view)> respondError)>;

class RpcChannel {
public:
    explicit RpcChannel(mini::net::EventLoop* loop);

    /// Called by the protocol adapter when data arrives on the connection.
    /// Tries to decode frames from the buffer and dispatch them.
    /// @return false if a malformed frame is detected (caller should close connection).
    bool onMessage(const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf);

    /// Send an RPC request and register a pending callback.
    /// @param conn     The connection to send on.
    /// @param method   The method to call.
    /// @param payload  The request payload.
    /// @param cb       Callback to invoke when response/error/timeout arrives.
    /// @param timeoutMs  Timeout in milliseconds (0 = no timeout).
    void sendRequest(const mini::net::TcpConnectionPtr& conn,
                     std::string_view method,
                     std::string_view payload,
                     RpcResponseCallback cb,
                     int timeoutMs = 0);

    /// Set the handler for incoming requests (server side).
    void setRequestCallback(RpcRequestCallback cb) { requestCallback_ = std::move(cb); }

    /// Fail all pending callbacks with an error (called on connection close).
    void failAllPending(const std::string& reason);

private:
    struct PendingCall {
        RpcResponseCallback callback;
        mini::net::TimerId timerId;
    };

    mini::net::EventLoop* loop_;
    std::uint64_t nextRequestId_{1};
    std::unordered_map<std::uint64_t, PendingCall> pendingCalls_;
    RpcRequestCallback requestCallback_;
};

}  // namespace mini::rpc
