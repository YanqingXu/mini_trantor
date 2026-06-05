#pragma once

// RpcClient 是 TcpClient 的 RPC 协议适配器。
// 它管理到单个 RPC 服务端的连接，提供 callback 和 coroutine 两种 call 接口。
// 所有 RPC 状态在连接的 owner loop 线程上管理。

#include "mini/rpc/RpcChannel.h"
#include "mini/coroutine/Task.h"
#include "mini/net/Callbacks.h"
#include "mini/net/TcpClient.h"

#include <chrono>
#include <coroutine>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace mini::net {
class EventLoop;
class InetAddress;
}  // namespace mini::net

namespace mini::rpc {

/// Exception thrown by coroCall() on RPC error or timeout.
class RpcError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Result of an RPC call.
struct RpcResult {
    std::string error;    // empty on success
    std::string payload;  // response payload (empty on error)
    bool ok() const noexcept { return error.empty(); }
};

class RpcClient {
public:
    RpcClient(mini::net::EventLoop* loop,
              const mini::net::InetAddress& serverAddr,
              std::string name);

    void connect();
    void disconnect();

    /// Callback-style RPC call.
    /// @param method    Service method name.
    /// @param payload   Request payload.
    /// @param cb        Callback invoked with (errorMsg, responsePayload).
    /// @param timeoutMs Timeout in milliseconds (0 = no timeout).
    void call(std::string_view method,
              std::string_view payload,
              RpcResponseCallback cb,
              int timeoutMs = 0);

    /// Coroutine-style RPC call.
    class CallAwaitable {
    public:
        CallAwaitable(RpcClient* client,
                      std::string method,
                      std::string payload,
                      int timeoutMs);

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle);
        RpcResult await_resume();

    private:
        RpcClient* client_;
        std::string method_;
        std::string payload_;
        int timeoutMs_;
        RpcResult result_;
    };

    /// co_await asyncCall("Method", payload, timeoutMs)
    CallAwaitable asyncCall(std::string method,
                            std::string payload,
                            int timeoutMs = 3000);

    /// Coroutine-style RPC call that returns payload directly.
    /// Throws RpcError on error or timeout.
    mini::coroutine::Task<std::string> coroCall(std::string method,
                                                std::string payload,
                                                int timeoutMs = 3000);

    void setConnectionCallback(mini::net::ConnectionCallback cb) {
        userConnectionCallback_ = std::move(cb);
    }

    bool connected() const;

private:
    void onConnection(const mini::net::TcpConnectionPtr& conn);
    void onMessage(const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf);

    mini::net::EventLoop* loop_;
    mini::net::TcpClient client_;
    mini::net::ConnectionCallback userConnectionCallback_;
};

}  // namespace mini::rpc
