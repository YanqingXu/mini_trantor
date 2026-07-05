#include "mini/net/broadcast/BroadcastRouter.h"

#include "mini/net/TcpConnection.h"

#include <mutex>
#include <utility>

namespace mini::net::broadcast {

BroadcastRouter::BroadcastRouter(EventLoop* baseLoop)
    : baseLoop_(baseLoop),
      lifetimeToken_(std::make_shared<int>(0)) {
}

BroadcastRouter::~BroadcastRouter() {
    lifetimeToken_.reset();
}

void BroadcastRouter::registerConnection(const TcpConnectionPtr& connection) {
    if (!connection) {
        return;
    }
    registerSession(connection->name(), connection);
}

void BroadcastRouter::registerSession(std::string sessionId, const TcpConnectionPtr& connection) {
    if (!baseLoop_ || sessionId.empty() || !connection) {
        return;
    }

    auto weakConnection = std::weak_ptr<TcpConnection>(connection);
    auto* loop = connection->getLoop();
    if (baseLoop_->isInLoopThread()) {
        registerInLoop(std::move(sessionId), std::move(weakConnection), {}, loop);
        return;
    }

    std::weak_ptr<void> lifetime = lifetimeToken_;
    baseLoop_->queueInLoop([this,
                            lifetime,
                            sessionId = std::move(sessionId),
                            weakConnection = std::move(weakConnection),
                            loop] {
        if (!lifetime.lock()) {
            return;
        }
        registerInLoop(std::move(sessionId), std::move(weakConnection), {}, loop);
    });
}

void BroadcastRouter::registerEndpoint(
    std::string sessionId,
    std::shared_ptr<transport::ITransportEndpoint> endpoint) {
    if (!baseLoop_ || sessionId.empty() || !endpoint) {
        return;
    }

    auto* loop = endpoint->getLoop();
    auto weakEndpoint = std::weak_ptr<transport::ITransportEndpoint>(endpoint);
    if (baseLoop_->isInLoopThread()) {
        registerInLoop(std::move(sessionId), {}, std::move(weakEndpoint), loop);
        return;
    }

    std::weak_ptr<void> lifetime = lifetimeToken_;
    baseLoop_->queueInLoop([this,
                            lifetime,
                            sessionId = std::move(sessionId),
                            weakEndpoint = std::move(weakEndpoint),
                            loop] {
        if (!lifetime.lock()) {
            return;
        }
        registerInLoop(std::move(sessionId), {}, std::move(weakEndpoint), loop);
    });
}

void BroadcastRouter::registerEndpoint(
    transport::TransportSessionId sessionId,
    std::shared_ptr<transport::ITransportEndpoint> endpoint) {
    registerEndpoint(transportSessionKey(sessionId), std::move(endpoint));
}

void BroadcastRouter::deregisterConnection(const TcpConnectionPtr& connection) {
    if (!baseLoop_ || !connection) {
        return;
    }

    auto expected = std::weak_ptr<TcpConnection>(connection);
    if (baseLoop_->isInLoopThread()) {
        deregisterConnectionInLoop(std::move(expected));
        return;
    }

    std::weak_ptr<void> lifetime = lifetimeToken_;
    baseLoop_->queueInLoop([this, lifetime, expected = std::move(expected)]() mutable {
        if (!lifetime.lock()) {
            return;
        }
        deregisterConnectionInLoop(std::move(expected));
    });
}

void BroadcastRouter::deregisterSession(std::string_view sessionId) {
    if (!baseLoop_ || sessionId.empty()) {
        return;
    }

    auto id = std::string(sessionId);
    if (baseLoop_->isInLoopThread()) {
        deregisterInLoop(std::move(id));
        return;
    }

    std::weak_ptr<void> lifetime = lifetimeToken_;
    baseLoop_->queueInLoop([this, lifetime, id = std::move(id)] {
        if (!lifetime.lock()) {
            return;
        }
        deregisterInLoop(id);
    });
}

void BroadcastRouter::deregisterSession(std::string_view sessionId,
                                        const TcpConnectionPtr& expectedConnection) {
    if (!baseLoop_ || sessionId.empty() || !expectedConnection) {
        return;
    }

    auto id = std::string(sessionId);
    auto expected = std::weak_ptr<TcpConnection>(expectedConnection);
    if (baseLoop_->isInLoopThread()) {
        deregisterIfConnectionMatchesInLoop(std::move(id), std::move(expected));
        return;
    }

    std::weak_ptr<void> lifetime = lifetimeToken_;
    baseLoop_->queueInLoop([this,
                            lifetime,
                            id = std::move(id),
                            expected = std::move(expected)]() mutable {
        if (!lifetime.lock()) {
            return;
        }
        deregisterIfConnectionMatchesInLoop(std::move(id), std::move(expected));
    });
}

void BroadcastRouter::joinGroup(std::string sessionId, std::string groupId) {
    if (!baseLoop_ || sessionId.empty() || groupId.empty()) {
        return;
    }
    if (baseLoop_->isInLoopThread()) {
        joinBucketInLoop(std::move(sessionId), std::move(groupId), sessionsByGroup_, groupsBySession_);
        return;
    }
    std::weak_ptr<void> lifetime = lifetimeToken_;
    baseLoop_->queueInLoop([this, lifetime, sessionId = std::move(sessionId), groupId = std::move(groupId)] {
        if (!lifetime.lock()) {
            return;
        }
        joinBucketInLoop(std::move(sessionId), std::move(groupId), sessionsByGroup_, groupsBySession_);
    });
}

void BroadcastRouter::leaveGroup(std::string_view sessionId, std::string_view groupId) {
    if (!baseLoop_ || sessionId.empty() || groupId.empty()) {
        return;
    }
    auto session = std::string(sessionId);
    auto group = std::string(groupId);
    if (baseLoop_->isInLoopThread()) {
        leaveBucketInLoop(std::move(session), std::move(group), sessionsByGroup_, groupsBySession_);
        return;
    }
    std::weak_ptr<void> lifetime = lifetimeToken_;
    baseLoop_->queueInLoop([this, lifetime, session = std::move(session), group = std::move(group)] {
        if (!lifetime.lock()) {
            return;
        }
        leaveBucketInLoop(std::move(session), std::move(group), sessionsByGroup_, groupsBySession_);
    });
}

void BroadcastRouter::joinAoi(std::string sessionId, std::string aoiId) {
    if (!baseLoop_ || sessionId.empty() || aoiId.empty()) {
        return;
    }
    if (baseLoop_->isInLoopThread()) {
        joinBucketInLoop(std::move(sessionId), std::move(aoiId), sessionsByAoi_, aoisBySession_);
        return;
    }
    std::weak_ptr<void> lifetime = lifetimeToken_;
    baseLoop_->queueInLoop([this, lifetime, sessionId = std::move(sessionId), aoiId = std::move(aoiId)] {
        if (!lifetime.lock()) {
            return;
        }
        joinBucketInLoop(std::move(sessionId), std::move(aoiId), sessionsByAoi_, aoisBySession_);
    });
}

void BroadcastRouter::leaveAoi(std::string_view sessionId, std::string_view aoiId) {
    if (!baseLoop_ || sessionId.empty() || aoiId.empty()) {
        return;
    }
    auto session = std::string(sessionId);
    auto aoi = std::string(aoiId);
    if (baseLoop_->isInLoopThread()) {
        leaveBucketInLoop(std::move(session), std::move(aoi), sessionsByAoi_, aoisBySession_);
        return;
    }
    std::weak_ptr<void> lifetime = lifetimeToken_;
    baseLoop_->queueInLoop([this, lifetime, session = std::move(session), aoi = std::move(aoi)] {
        if (!lifetime.lock()) {
            return;
        }
        leaveBucketInLoop(std::move(session), std::move(aoi), sessionsByAoi_, aoisBySession_);
    });
}

void BroadcastRouter::registerInLoop(
    std::string sessionId,
    std::weak_ptr<TcpConnection> connection,
    std::weak_ptr<transport::ITransportEndpoint> endpoint,
    EventLoop* loop) {
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);

    auto existing = sessionById_.find(sessionId);
    if (existing != sessionById_.end()) {
        const auto oldLoop = existing->second.loop;
        if (oldLoop != nullptr) {
            auto loopIt = sessionsByLoop_.find(oldLoop);
            if (loopIt != sessionsByLoop_.end()) {
                loopIt->second.erase(sessionId);
                pruneLoopBucket(oldLoop);
            }
        }
    }

    sessionById_[sessionId] = {std::move(connection), std::move(endpoint), loop};
    sessionsByLoop_[loop].insert(sessionId);
}

void BroadcastRouter::deregisterInLoop(std::string sessionId) {
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);

    auto sessionIt = sessionById_.find(sessionId);
    if (sessionIt == sessionById_.end()) {
        return;
    }

    const auto sessionLoop = sessionIt->second.loop;
    sessionById_.erase(sessionIt);

    if (sessionLoop != nullptr) {
        auto loopIt = sessionsByLoop_.find(sessionLoop);
        if (loopIt != sessionsByLoop_.end()) {
            loopIt->second.erase(sessionId);
            pruneLoopBucket(sessionLoop);
        }
    }

    auto eraseFromBuckets = [&](auto& sessionsByBucket, auto& bucketsBySession) {
        auto bucketIt = bucketsBySession.find(sessionId);
        if (bucketIt == bucketsBySession.end()) {
            return;
        }
        for (const auto& bucket : bucketIt->second) {
            auto membersIt = sessionsByBucket.find(bucket);
            if (membersIt == sessionsByBucket.end()) {
                continue;
            }
            membersIt->second.erase(sessionId);
            if (membersIt->second.empty()) {
                sessionsByBucket.erase(membersIt);
            }
        }
        bucketsBySession.erase(bucketIt);
    };
    eraseFromBuckets(sessionsByGroup_, groupsBySession_);
    eraseFromBuckets(sessionsByAoi_, aoisBySession_);
}

void BroadcastRouter::deregisterIfConnectionMatchesInLoop(
    std::string sessionId,
    std::weak_ptr<TcpConnection> expectedConnection) {
    baseLoop_->assertInLoopThread();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto sessionIt = sessionById_.find(sessionId);
        if (sessionIt == sessionById_.end()) {
            return;
        }

        auto current = sessionIt->second.connection.lock();
        auto expected = expectedConnection.lock();
        if (!current || !expected || current != expected) {
            return;
        }
    }

    deregisterInLoop(std::move(sessionId));
}

void BroadcastRouter::deregisterConnectionInLoop(std::weak_ptr<TcpConnection> expectedConnection) {
    baseLoop_->assertInLoopThread();

    auto expected = expectedConnection.lock();
    if (!expected) {
        return;
    }

    std::vector<std::string> matchingSessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [sessionId, record] : sessionById_) {
            auto current = record.connection.lock();
            if (current && current == expected) {
                matchingSessions.push_back(sessionId);
            }
        }
    }

    for (auto& sessionId : matchingSessions) {
        deregisterInLoop(std::move(sessionId));
    }
}

void BroadcastRouter::joinBucketInLoop(
    std::string sessionId,
    std::string bucketId,
    std::unordered_map<std::string, std::unordered_set<std::string>>& sessionsByBucket,
    std::unordered_map<std::string, std::unordered_set<std::string>>& bucketsBySession) {
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessionById_.find(sessionId) == sessionById_.end()) {
        return;
    }
    sessionsByBucket[bucketId].insert(sessionId);
    bucketsBySession[sessionId].insert(std::move(bucketId));
}

void BroadcastRouter::leaveBucketInLoop(
    std::string sessionId,
    std::string bucketId,
    std::unordered_map<std::string, std::unordered_set<std::string>>& sessionsByBucket,
    std::unordered_map<std::string, std::unordered_set<std::string>>& bucketsBySession) {
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);

    auto membersIt = sessionsByBucket.find(bucketId);
    if (membersIt != sessionsByBucket.end()) {
        membersIt->second.erase(sessionId);
        if (membersIt->second.empty()) {
            sessionsByBucket.erase(membersIt);
        }
    }

    auto bucketsIt = bucketsBySession.find(sessionId);
    if (bucketsIt != bucketsBySession.end()) {
        bucketsIt->second.erase(bucketId);
        if (bucketsIt->second.empty()) {
            bucketsBySession.erase(bucketsIt);
        }
    }
}

std::vector<BroadcastRouter::LoopBatch> BroadcastRouter::route(const SessionIds& sessionIds) const {
    if (!baseLoop_) {
        return {};
    }
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);
    return routeLocked(sessionIds);
}

std::vector<BroadcastRouter::LoopBatch> BroadcastRouter::routeGroup(std::string_view groupId) const {
    if (!baseLoop_) {
        return {};
    }
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);
    return routeBucketLocked(groupId, sessionsByGroup_);
}

std::vector<BroadcastRouter::LoopBatch> BroadcastRouter::routeAoi(std::string_view aoiId) const {
    if (!baseLoop_) {
        return {};
    }
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);
    return routeBucketLocked(aoiId, sessionsByAoi_);
}

std::vector<BroadcastRouter::LoopBatch> BroadcastRouter::routeAll() const {
    if (!baseLoop_) {
        return {};
    }
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);

    SessionIds all;
    all.reserve(sessionById_.size());
    for (const auto& [id, record] : sessionById_) {
        (void)record;
        all.push_back(id);
    }
    return routeLocked(all);
}

std::vector<BroadcastRouter::LoopBatch>
BroadcastRouter::routeLocked(const SessionIds& sessionIds) const {
    std::vector<LoopBatch> result;
    if (sessionIds.empty()) {
        return result;
    }

    std::unordered_set<std::string> dedup;
    dedup.reserve(sessionIds.size());

    std::unordered_map<EventLoop*, std::size_t> indexByLoop;
    indexByLoop.reserve(sessionById_.size());

    for (const auto& id : sessionIds) {
        if (id.empty() || !dedup.insert(id).second) {
            continue;
        }

        auto it = sessionById_.find(id);
        if (it == sessionById_.end()) {
            continue;
        }

        auto endpoint = it->second.endpoint.lock();
        auto connection = it->second.connection.lock();
        if (!endpoint && !connection) {
            continue;
        }

        auto* loop = endpoint ? endpoint->getLoop() : it->second.loop;
        if (loop == nullptr && connection) {
            loop = connection->getLoop();
        }
        if (loop == nullptr) {
            continue;
        }

        auto idxIt = indexByLoop.find(loop);
        if (idxIt == indexByLoop.end()) {
            indexByLoop.emplace(loop, result.size());
            result.push_back(LoopBatch{loop, {}, {}});
            idxIt = indexByLoop.find(loop);
        }

        auto& batch = result[idxIt->second];
        if (endpoint) {
            batch.endpoints.push_back(std::move(endpoint));
        } else {
            batch.connections.push_back(std::move(connection));
        }
    }

    return result;
}

std::vector<BroadcastRouter::LoopBatch> BroadcastRouter::routeBucketLocked(
    std::string_view bucketId,
    const std::unordered_map<std::string, std::unordered_set<std::string>>& sessionsByBucket) const {
    auto bucketIt = sessionsByBucket.find(std::string(bucketId));
    if (bucketIt == sessionsByBucket.end()) {
        return {};
    }

    SessionIds sessionIds;
    sessionIds.reserve(bucketIt->second.size());
    for (const auto& id : bucketIt->second) {
        sessionIds.push_back(id);
    }
    return routeLocked(sessionIds);
}

std::size_t BroadcastRouter::sessionCount() const {
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);
    return sessionById_.size();
}

bool BroadcastRouter::hasSession(std::string_view sessionId) const {
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);
    return sessionById_.find(std::string(sessionId)) != sessionById_.end();
}

bool BroadcastRouter::hasGroupMember(std::string_view groupId, std::string_view sessionId) const {
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessionsByGroup_.find(std::string(groupId));
    return it != sessionsByGroup_.end() &&
           it->second.find(std::string(sessionId)) != it->second.end();
}

bool BroadcastRouter::hasAoiMember(std::string_view aoiId, std::string_view sessionId) const {
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessionsByAoi_.find(std::string(aoiId));
    return it != sessionsByAoi_.end() &&
           it->second.find(std::string(sessionId)) != it->second.end();
}

std::size_t BroadcastRouter::loopBucketCount() const {
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);
    return sessionsByLoop_.size();
}

void BroadcastRouter::pruneLoopBucket(EventLoop* loop) {
    auto it = sessionsByLoop_.find(loop);
    if (it == sessionsByLoop_.end()) {
        return;
    }
    if (it->second.empty()) {
        sessionsByLoop_.erase(it);
    }
}

std::string BroadcastRouter::transportSessionKey(transport::TransportSessionId sessionId) {
    return std::to_string(sessionId);
}

}  // namespace mini::net::broadcast
