#pragma once

// RpcConnectionPool 是 RPC 客户端连接池。
// 运行规则：
// - 池中连接按 owner EventLoop 单线程调度；
// - 请求在池内 round-robin 分发到空闲连接；
// - 连接断开后可视情况重连并续发待发送/在飞失败请求；
// - stop() 明确 fail 全部 pending 与 in-flight 请求。

#include "mini/base/noncopyable.h"
#include "mini/net/Callbacks.h"
#include "mini/rpc/RpcChannel.h"
#include "mini/rpc/RpcClient.h"
#include "mini/rpc/RpcPoolOptions.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mini::net {
class EventLoop;
class InetAddress;
class TcpConnection;
}  // namespace mini::net

namespace mini::rpc {

class RpcConnectionPool : private mini::base::noncopyable {
public:
    RpcConnectionPool(mini::net::EventLoop* loop,
                      const mini::net::InetAddress& serverAddr,
                      std::string name,
                      RpcPoolOptions options = {});

    void start();
    void stop();

    /// Callback-style RPC request.
    void call(std::string_view method,
              std::string_view payload,
              RpcResponseCallback cb,
              int timeoutMs = 0);

    void setConnectionCallback(mini::net::ConnectionCallback cb) {
        userConnectionCallback_ = std::move(cb);
    }

private:
    struct PendingCall {
        std::string method;
        std::string payload;
        RpcResponseCallback callback;
        int timeoutMs{0};
        bool done{false};
    };

    struct PoolEntry {
        std::unique_ptr<RpcClient> client;
        bool connected{false};
        bool inUse{false};
        std::shared_ptr<PendingCall> inFlight;
    };

    void createInitialEntries();
    void createEntry();
    void startEntry(std::size_t idx);
    void ensureEntries(std::size_t target);
    void runInLoopAuto(std::function<void()> fn);
    void onConnection(std::size_t idx, const mini::net::TcpConnectionPtr& conn);
    void dispatch();
    std::size_t nextConnectedEntry();
    void dispatchOne(std::size_t idx);
    void onCallComplete(std::size_t idx,
                        std::shared_ptr<PendingCall> req,
                        const std::string& error,
                        const std::string& payload);
    void failPending(std::string reason);
    void failOne(std::shared_ptr<PendingCall> req, const std::string& err, std::string payload);
    bool shouldRetry(std::string_view error) const;

    bool stopped_{false};
    bool started_{false};
    bool ensureEntriesInProgress_{false};
    std::size_t nextRR_{0};

    mini::net::EventLoop* loop_;
    mini::net::InetAddress serverAddr_;
    std::string name_;
    RpcPoolOptions options_;
    mini::net::ConnectionCallback userConnectionCallback_;

    std::vector<PoolEntry> entries_;
    std::deque<std::shared_ptr<PendingCall>> pendingRequests_;
};

}  // namespace mini::rpc
