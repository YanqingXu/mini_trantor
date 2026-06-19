#include "mini/rpc/RpcConnectionPool.h"

#include "mini/net/EventLoop.h"
#include "mini/net/TcpConnection.h"

#include <functional>

namespace mini::rpc {

RpcConnectionPool::RpcConnectionPool(mini::net::EventLoop* loop,
                                     const mini::net::InetAddress& serverAddr,
                                     std::string name,
                                     RpcPoolOptions options)
    : loop_(loop),
      serverAddr_(serverAddr),
      name_(std::move(name)),
      options_(std::move(options)) {
    options_.validate();
    entries_.reserve(options_.maxConnections);
}

void RpcConnectionPool::runInLoopAuto(std::function<void()> fn) {
    if (loop_->isInLoopThread()) {
        fn();
    } else {
        loop_->queueInLoop(std::move(fn));
    }
}

void RpcConnectionPool::start() {
    runInLoopAuto([this] {
        if (stopped_ || started_) {
            return;
        }
        started_ = true;
        createInitialEntries();
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            startEntry(i);
        }
    });
}

void RpcConnectionPool::createEntry() {
    if (entries_.size() >= options_.maxConnections) {
        return;
    }

    const auto idx = entries_.size();
    mini::net::TcpClientOptions clientOptions;
    clientOptions.retry = options_.connector.enableRetry;
    clientOptions.connector = options_.connector;
    entries_.push_back(PoolEntry{});
    entries_.back().client = std::make_unique<RpcClient>(
        loop_,
        serverAddr_,
        name_ + "#pool-" + std::to_string(idx + 1),
        std::move(clientOptions));

    entries_.back().client->setConnectionCallback(
        [this, idx](const mini::net::TcpConnectionPtr& conn) { onConnection(idx, conn); });
}

void RpcConnectionPool::ensureEntries(std::size_t target) {
    while (entries_.size() < target && entries_.size() < options_.maxConnections) {
        createEntry();
    }
}

void RpcConnectionPool::createInitialEntries() {
    ensureEntries(options_.minConnections);
}

void RpcConnectionPool::startEntry(std::size_t idx) {
    if (idx >= entries_.size()) {
        return;
    }
    entries_[idx].connected = false;
    entries_[idx].inUse = false;
    entries_[idx].inFlight.reset();
    entries_[idx].client->connect();
}

void RpcConnectionPool::stop() {
    runInLoopAuto([this] {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        started_ = false;
        failPending("pool stopped");

        for (auto& entry : entries_) {
            entry.connected = false;
            entry.inUse = false;
            entry.inFlight.reset();
            entry.client->disconnect();
        }
    });
}

void RpcConnectionPool::call(std::string_view method,
                            std::string_view payload,
                            RpcResponseCallback cb,
                            int timeoutMs) {
    runInLoopAuto([this, method = std::string(method), payload = std::string(payload),
                   timeoutMs, cb = std::move(cb)]() mutable {
        if (stopped_) {
            if (cb) {
                cb("pool stopped", "");
            }
            return;
        }

        auto req = std::make_shared<PendingCall>();
        req->method = std::move(method);
        req->payload = std::move(payload);
        req->callback = std::move(cb);
        req->timeoutMs = timeoutMs;
        pendingRequests_.push_back(std::move(req));

        if (entries_.empty() && options_.createOnDemand) {
            createEntry();
            startEntry(entries_.size() - 1);
        }

        dispatch();
    });
}

void RpcConnectionPool::onConnection(std::size_t idx, const mini::net::TcpConnectionPtr& conn) {
    if (idx >= entries_.size()) {
        return;
    }

    auto& entry = entries_[idx];
    const bool hadInFlight = static_cast<bool>(entry.inFlight);
    entry.connected = conn && conn->connected();

    if (userConnectionCallback_) {
        userConnectionCallback_(conn);
    }

    if (stopped_) {
        return;
    }

    if (!conn || !conn->connected()) {
        entry.inUse = false;
        entry.inFlight.reset();
        // Reconnect on demand when the pool still has observable work.
        if (options_.connector.enableRetry) {
            return;
        }
        if (!pendingRequests_.empty() || hadInFlight) {
            entry.client->connect();
        }
        return;
    }

    dispatch();
}

std::size_t RpcConnectionPool::nextConnectedEntry() {
    if (entries_.empty()) {
        return static_cast<std::size_t>(-1);
    }

    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const auto idx = (nextRR_ + i) % entries_.size();
        const auto& entry = entries_[idx];
        if (entry.connected && !entry.inUse && entry.client->connected()) {
            nextRR_ = (idx + 1) % entries_.size();
            return idx;
        }
    }
    return static_cast<std::size_t>(-1);
}

void RpcConnectionPool::dispatchOne(std::size_t idx) {
    if (idx >= entries_.size() || pendingRequests_.empty()) {
        return;
    }

    auto& entry = entries_[idx];
    if (!entry.connected || entry.inUse || !entry.client->connected() || pendingRequests_.empty()) {
        return;
    }

    auto req = std::move(pendingRequests_.front());
    pendingRequests_.pop_front();
    entry.inUse = true;
    entry.inFlight = req;
    entry.client->call(req->method, req->payload,
                       [this, idx, req](const std::string& error, const std::string& payload) {
                           onCallComplete(idx, req, error, payload);
                       },
                       req->timeoutMs);
}

void RpcConnectionPool::dispatch() {
    if (stopped_ || pendingRequests_.empty()) {
        return;
    }

    bool dispatched = true;
    while (dispatched && !pendingRequests_.empty()) {
        dispatched = false;

        auto idx = nextConnectedEntry();
        if (idx != static_cast<std::size_t>(-1)) {
            dispatchOne(idx);
            dispatched = true;
            continue;
        }

        // 没有可用连接且开启了按需创建，则尝试创建一条新连接补位。
        if (options_.createOnDemand && entries_.size() < options_.maxConnections) {
            if (!ensureEntriesInProgress_) {
                ensureEntriesInProgress_ = true;
                createEntry();
                startEntry(entries_.size() - 1);
            }
            break;
        }
        break;
    }
    ensureEntriesInProgress_ = false;
}

bool RpcConnectionPool::shouldRetry(std::string_view error) const {
    return error == "connection closed" || error == "not connected" ||
           error == "connection reset";
}

void RpcConnectionPool::failOne(std::shared_ptr<PendingCall> req,
                               const std::string& err,
                               std::string payload) {
    if (!req || req->done) {
        return;
    }
    req->done = true;
    if (req->callback) {
        req->callback(err, std::move(payload));
    }
}

void RpcConnectionPool::onCallComplete(std::size_t idx,
                                     std::shared_ptr<PendingCall> req,
                                     const std::string& error,
                                     const std::string& payload) {
    if (idx >= entries_.size()) {
        if (!req->done) {
            failOne(std::move(req), error.empty() ? "pool internal error" : error, payload);
        }
        return;
    }

    auto& entry = entries_[idx];
    if (entry.inFlight == req) {
        entry.inFlight.reset();
    }
    entry.inUse = false;

    if (req->done) {
        return;
    }

    if (error.empty()) {
        failOne(std::move(req), "", payload);
    } else if (!stopped_ && shouldRetry(error)) {
        if (!error.empty()) {
            pendingRequests_.push_front(std::move(req));
        }
    } else {
        failOne(std::move(req), error, {});
    }

    dispatch();
}

void RpcConnectionPool::failPending(std::string reason) {
    while (!pendingRequests_.empty()) {
        auto req = std::move(pendingRequests_.front());
        pendingRequests_.pop_front();
        failOne(std::move(req), std::move(reason), {});
    }

    for (auto& entry : entries_) {
        entry.inUse = false;
        if (entry.inFlight) {
            failOne(std::move(entry.inFlight), reason, {});
        }
        entry.inFlight.reset();
    }
}

}  // namespace mini::rpc
