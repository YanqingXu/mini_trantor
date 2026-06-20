// BroadcastRouter — base-loop 侧广播路由器（Task-04）。
//
// 目标：
// - base loop 维护“会话-IO loop”索引，避免每次广播都遍历 TcpServer::connections_ 全量 map；
// - 按目标会话集合做 ioLoop 分桶，减少跨循环 queue 次数（按 loop 一次入队）；
// - 仅持有会话弱引用，避免与 TcpConnection 形成生命周期环。

#pragma once

#include "mini/base/noncopyable.h"
#include "mini/net/Callbacks.h"
#include "mini/net/EventLoop.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>

namespace mini::net {

namespace broadcast {

class BroadcastRouter : private mini::base::noncopyable {
public:
    struct LoopBatch {
        EventLoop* loop{nullptr};
        std::vector<TcpConnectionPtr> connections;
    };

    using SessionIds = std::vector<std::string>;

    explicit BroadcastRouter(EventLoop* baseLoop);

    void registerConnection(const TcpConnectionPtr& connection);
    void deregisterConnection(const TcpConnectionPtr& connection);

    std::vector<LoopBatch> route(const SessionIds& sessionIds) const;
    std::vector<LoopBatch> routeAll() const;

    std::size_t sessionCount() const;
    bool hasSession(std::string_view sessionId) const;
    std::size_t loopBucketCount() const;

private:
    struct SessionRecord {
        std::weak_ptr<TcpConnection> connection;
        EventLoop* loop{nullptr};
    };

    void registerInLoop(std::string sessionId, std::weak_ptr<TcpConnection> connection, EventLoop* loop);
    void deregisterInLoop(std::string sessionId);
    void pruneLoopBucket(EventLoop* loop);

    EventLoop* baseLoop_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, SessionRecord> sessionById_;
    std::unordered_map<EventLoop*, std::unordered_set<std::string>> sessionsByLoop_;
};

}  // namespace broadcast

}  // namespace mini::net
