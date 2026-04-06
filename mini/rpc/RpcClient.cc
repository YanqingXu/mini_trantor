#include "mini/rpc/RpcClient.h"

#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/TcpConnection.h"

#include <any>
#include <utility>

namespace mini::rpc {

RpcClient::RpcClient(mini::net::EventLoop* loop,
                     const mini::net::InetAddress& serverAddr,
                     std::string name)
    : loop_(loop),
      client_(loop, serverAddr, std::move(name)) {
    client_.setConnectionCallback(
        [this](const mini::net::TcpConnectionPtr& conn) { onConnection(conn); });
    client_.setMessageCallback(
        [this](const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
            onMessage(conn, buf);
        });
}

void RpcClient::connect() {
    client_.connect();
}

void RpcClient::disconnect() {
    client_.disconnect();
}

void RpcClient::call(std::string_view method,
                     std::string_view payload,
                     RpcResponseCallback cb,
                     int timeoutMs) {
    auto action = [this, m = std::string(method), p = std::string(payload),
                   cb = std::move(cb), timeoutMs]() mutable {
        auto conn = client_.connection();
        if (!conn || !conn->connected()) {
            if (cb) {
                cb("not connected", "");
            }
            return;
        }
        auto* channel = std::any_cast<RpcChannel>(&conn->getContext());
        if (!channel) {
            if (cb) {
                cb("no RPC channel", "");
            }
            return;
        }
        channel->sendRequest(conn, m, p, std::move(cb), timeoutMs);
    };

    if (loop_->isInLoopThread()) {
        action();
    } else {
        loop_->queueInLoop(std::move(action));
    }
}

bool RpcClient::connected() const {
    auto conn = client_.connection();
    return conn && conn->connected();
}

void RpcClient::onConnection(const mini::net::TcpConnectionPtr& conn) {
    if (conn->connected()) {
        conn->setContext(RpcChannel(conn->getLoop()));
    } else {
        auto* channel = std::any_cast<RpcChannel>(&conn->getContext());
        if (channel) {
            channel->failAllPending("connection closed");
        }
    }
    if (userConnectionCallback_) {
        userConnectionCallback_(conn);
    }
}

void RpcClient::onMessage(const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
    auto* channel = std::any_cast<RpcChannel>(&conn->getContext());
    if (!channel) {
        conn->forceClose();
        return;
    }
    if (!channel->onMessage(conn, buf)) {
        conn->forceClose();
    }
}

// --- CallAwaitable ---

RpcClient::CallAwaitable::CallAwaitable(RpcClient* client,
                                         std::string method,
                                         std::string payload,
                                         int timeoutMs)
    : client_(client),
      method_(std::move(method)),
      payload_(std::move(payload)),
      timeoutMs_(timeoutMs) {
}

void RpcClient::CallAwaitable::await_suspend(std::coroutine_handle<> handle) {
    client_->call(method_, payload_,
        [this, handle](const std::string& error, const std::string& payload) {
            result_.error = error;
            result_.payload = payload;
            handle.resume();
        },
        timeoutMs_);
}

RpcResult RpcClient::CallAwaitable::await_resume() {
    return std::move(result_);
}

RpcClient::CallAwaitable RpcClient::asyncCall(std::string method,
                                               std::string payload,
                                               int timeoutMs) {
    return CallAwaitable(this, std::move(method), std::move(payload), timeoutMs);
}

mini::coroutine::Task<std::string> RpcClient::coroCall(std::string method,
                                                        std::string payload,
                                                        int timeoutMs) {
    auto result = co_await asyncCall(std::move(method), std::move(payload), timeoutMs);
    if (!result.ok()) {
        throw RpcError(result.error);
    }
    co_return std::move(result.payload);
}

}  // namespace mini::rpc
