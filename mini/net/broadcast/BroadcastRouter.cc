#include "mini/net/broadcast/BroadcastRouter.h"

#include "mini/net/TcpConnection.h"

#include <mutex>

namespace mini::net::broadcast {

BroadcastRouter::BroadcastRouter(EventLoop* baseLoop)
    : baseLoop_(baseLoop) {
}

void BroadcastRouter::registerConnection(const TcpConnectionPtr& connection) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connection) {
        return;
    }
    if (!baseLoop_) {
        return;
    }

    std::string sessionId = connection->name();
    auto loop = connection->getLoop();
    auto weakConnection = std::weak_ptr<TcpConnection>(connection);

    if (baseLoop_->isInLoopThread()) {
        registerInLoop(std::move(sessionId), std::move(weakConnection), loop);
        return;
    }

    baseLoop_->queueInLoop([this,
                            sessionId = std::move(sessionId),
                            weakConnection = std::move(weakConnection),
                            loop = loop] {
        registerInLoop(std::move(sessionId), std::move(weakConnection), loop);
    });
}

void BroadcastRouter::deregisterConnection(const TcpConnectionPtr& connection) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connection) {
        return;
    }
    if (!baseLoop_) {
        return;
    }

    std::string sessionId = connection->name();

    if (baseLoop_->isInLoopThread()) {
        deregisterInLoop(std::move(sessionId));
        return;
    }

    baseLoop_->queueInLoop([this, sessionId = std::move(sessionId)] { deregisterInLoop(sessionId); });
}

void BroadcastRouter::registerInLoop(std::string sessionId,
                                    std::weak_ptr<TcpConnection> connection,
                                    EventLoop* loop) {
    baseLoop_->assertInLoopThread();
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

    sessionById_[sessionId] = {std::move(connection), loop};
    sessionsByLoop_[loop].insert(sessionId);
}

void BroadcastRouter::deregisterInLoop(std::string sessionId) {
    baseLoop_->assertInLoopThread();
    auto sessionIt = sessionById_.find(sessionId);
    if (sessionIt == sessionById_.end()) {
        return;
    }

    const auto sessionLoop = sessionIt->second.loop;
    sessionById_.erase(sessionIt);

    if (sessionLoop == nullptr) {
        return;
    }

    auto loopIt = sessionsByLoop_.find(sessionLoop);
    if (loopIt == sessionsByLoop_.end()) {
        return;
    }

    loopIt->second.erase(sessionId);
    pruneLoopBucket(sessionLoop);
}

std::vector<BroadcastRouter::LoopBatch> BroadcastRouter::route(const SessionIds& sessionIds) const {
    if (!baseLoop_) {
        return {};
    }
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<LoopBatch> result;
    if (sessionIds.empty()) {
        return result;
    }

    std::unordered_set<std::string_view> dedup;
    dedup.reserve(sessionIds.size());

    std::unordered_map<EventLoop*, std::size_t> indexByLoop;
    indexByLoop.reserve(sessionById_.size());

    for (const auto& id : sessionIds) {
        const auto idView = std::string_view(id);
        if (id.empty() || dedup.find(idView) != dedup.end()) {
            continue;
        }
        dedup.insert(idView);

        auto it = sessionById_.find(id);
        if (it == sessionById_.end()) {
            continue;
        }

        auto session = it->second.connection.lock();
        if (!session) {
            continue;
        }

        auto* loop = it->second.loop;
        auto idxIt = indexByLoop.find(loop);
        if (idxIt == indexByLoop.end()) {
            indexByLoop.emplace(loop, result.size());
            result.push_back(LoopBatch{loop, {session}});
        } else {
            result[idxIt->second].connections.push_back(session);
        }
    }

    return result;
}

std::vector<BroadcastRouter::LoopBatch> BroadcastRouter::routeAll() const {
    if (!baseLoop_) {
        return {};
    }
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<LoopBatch> result;
    result.reserve(sessionsByLoop_.size());

    std::unordered_map<EventLoop*, std::size_t> indexByLoop;
    indexByLoop.reserve(sessionsByLoop_.size());

    for (const auto& [loop, ids] : sessionsByLoop_) {
        auto idxIt = indexByLoop.find(loop);
        auto idx = idxIt;
        if (idxIt == indexByLoop.end()) {
            const auto insertedIndex = result.size();
            indexByLoop.emplace(loop, insertedIndex);
            result.push_back(LoopBatch{loop, {}});
            idx = indexByLoop.find(loop);
        }

        auto& batch = result[idx->second];
        for (const auto& id : ids) {
            auto sessionIt = sessionById_.find(id);
            if (sessionIt == sessionById_.end()) {
                continue;
            }

            auto session = sessionIt->second.connection.lock();
            if (session) {
                batch.connections.push_back(session);
            }
        }
    }

    return result;
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

std::size_t BroadcastRouter::loopBucketCount() const {
    baseLoop_->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);
    return sessionsByLoop_.size();
}

void BroadcastRouter::pruneLoopBucket(EventLoop* loop) {
    baseLoop_->assertInLoopThread();
    auto it = sessionsByLoop_.find(loop);
    if (it == sessionsByLoop_.end()) {
        return;
    }
    if (it->second.empty()) {
        sessionsByLoop_.erase(it);
    }
}

}  // namespace mini::net::broadcast
