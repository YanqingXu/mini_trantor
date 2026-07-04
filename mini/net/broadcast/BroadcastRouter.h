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
#include "mini/net/transport/ITransport.h"

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
        std::vector<std::shared_ptr<transport::ITransportEndpoint>> endpoints;
    };

    using SessionIds = std::vector<std::string>;

    explicit BroadcastRouter(EventLoop* baseLoop);
    ~BroadcastRouter();

    void registerConnection(const TcpConnectionPtr& connection);
    void registerSession(std::string sessionId, const TcpConnectionPtr& connection);
    void registerEndpoint(std::string sessionId, std::shared_ptr<transport::ITransportEndpoint> endpoint);
    void registerEndpoint(transport::TransportSessionId sessionId,
                          std::shared_ptr<transport::ITransportEndpoint> endpoint);
    void deregisterConnection(const TcpConnectionPtr& connection);
    void deregisterSession(std::string_view sessionId);
    void deregisterSession(std::string_view sessionId, const TcpConnectionPtr& expectedConnection);

    void joinGroup(std::string sessionId, std::string groupId);
    void leaveGroup(std::string_view sessionId, std::string_view groupId);
    void joinAoi(std::string sessionId, std::string aoiId);
    void leaveAoi(std::string_view sessionId, std::string_view aoiId);

    std::vector<LoopBatch> route(const SessionIds& sessionIds) const;
    std::vector<LoopBatch> routeGroup(std::string_view groupId) const;
    std::vector<LoopBatch> routeAoi(std::string_view aoiId) const;
    std::vector<LoopBatch> routeAll() const;

    std::size_t sessionCount() const;
    bool hasSession(std::string_view sessionId) const;
    bool hasGroupMember(std::string_view groupId, std::string_view sessionId) const;
    bool hasAoiMember(std::string_view aoiId, std::string_view sessionId) const;
    std::size_t loopBucketCount() const;

private:
    struct SessionRecord {
        std::weak_ptr<TcpConnection> connection;
        std::weak_ptr<transport::ITransportEndpoint> endpoint;
        EventLoop* loop{nullptr};
    };

    void registerInLoop(std::string sessionId,
                        std::weak_ptr<TcpConnection> connection,
                        std::weak_ptr<transport::ITransportEndpoint> endpoint,
                        EventLoop* loop);
    void deregisterInLoop(std::string sessionId);
    void deregisterIfConnectionMatchesInLoop(std::string sessionId,
                                             std::weak_ptr<TcpConnection> expectedConnection);
    void joinBucketInLoop(std::string sessionId,
                          std::string bucketId,
                          std::unordered_map<std::string, std::unordered_set<std::string>>& sessionsByBucket,
                          std::unordered_map<std::string, std::unordered_set<std::string>>& bucketsBySession);
    void leaveBucketInLoop(std::string sessionId,
                           std::string bucketId,
                           std::unordered_map<std::string, std::unordered_set<std::string>>& sessionsByBucket,
                           std::unordered_map<std::string, std::unordered_set<std::string>>& bucketsBySession);
    std::vector<LoopBatch> routeLocked(const SessionIds& sessionIds) const;
    std::vector<LoopBatch> routeBucketLocked(
        std::string_view bucketId,
        const std::unordered_map<std::string, std::unordered_set<std::string>>& sessionsByBucket) const;
    void pruneLoopBucket(EventLoop* loop);
    static std::string transportSessionKey(transport::TransportSessionId sessionId);

    EventLoop* baseLoop_;
    std::shared_ptr<void> lifetimeToken_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, SessionRecord> sessionById_;
    std::unordered_map<EventLoop*, std::unordered_set<std::string>> sessionsByLoop_;
    std::unordered_map<std::string, std::unordered_set<std::string>> sessionsByGroup_;
    std::unordered_map<std::string, std::unordered_set<std::string>> groupsBySession_;
    std::unordered_map<std::string, std::unordered_set<std::string>> sessionsByAoi_;
    std::unordered_map<std::string, std::unordered_set<std::string>> aoisBySession_;
};

}  // namespace broadcast

}  // namespace mini::net
