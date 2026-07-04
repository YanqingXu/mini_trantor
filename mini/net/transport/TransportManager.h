#pragma once

// TransportManager — 线程安全地注册/管理 TransportEndpoint 生命周期与发包入口。
//
// 设计目标：
// - Endpoint 的 stateful 对象可按 sessionId 查询、发送、关闭。
// - 任意线程可调用 send/close/register/deregister；非 owner loop 调用会通过 queueInLoop 回流。
// - 管理层持有 shared_ptr，避免重复创建与悬挂引用；上层 Session 侧默认持有 weak_ptr。

#include "mini/net/transport/ITransport.h"
#include "mini/net/transport/TransportEndpoint.h"
#include "mini/net/EventLoop.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace mini::net {
class TcpConnection;
}

namespace mini::net::transport {

class TransportManager {
public:
    explicit TransportManager(mini::net::EventLoop* ownerLoop)
        : ownerLoop_(ownerLoop),
          lifetimeToken_(std::make_shared<int>(0)),
          nextSessionId_(kFirstTransportSessionId) {
    }

    ~TransportManager() {
        lifetimeToken_.reset();
    }

    TransportSessionId registerEndpoint(const std::shared_ptr<ITransportEndpoint>& endpoint) {
        if (!endpoint) {
            return kInvalidTransportSessionId;
        }

        const auto id = endpoint->sessionId() == kInvalidTransportSessionId
            ? nextSessionId_.fetch_add(1, std::memory_order_relaxed)
            : endpoint->sessionId();
        endpoint->setSessionId(id);
        post([this, endpoint, id] {
            std::scoped_lock lock(mutex_);
            endpoints_[id] = endpoint;
        });
        return id;
    }

    TransportSessionId registerConnection(const std::shared_ptr<mini::net::TcpConnection>& conn,
                                        TransportKind kind = TransportKind::kTcp) {
        if (!conn) {
            return kInvalidTransportSessionId;
        }

        const auto endpoint = TransportEndpoint::create(conn, kInvalidTransportSessionId, kind);
        return registerEndpoint(endpoint);
    }

    bool deregisterEndpoint(TransportSessionId id) {
        if (id == kInvalidTransportSessionId) {
            return false;
        }
        post([this, id] {
            std::scoped_lock lock(mutex_);
            endpoints_.erase(id);
        });
        return true;
    }

    void send(TransportSessionId id, std::string_view data) {
        std::string payload(data);
        post([this, id, payload = std::move(payload)] {
            auto endpoint = getEndpointLocked(id);
            if (endpoint) {
                endpoint->send(payload);
            }
        });
    }

    void close(TransportSessionId id) {
        post([this, id] {
            auto endpoint = getEndpointLocked(id);
            if (endpoint) {
                endpoint->forceClose();
            }
        });
    }

    std::size_t endpointCount() const {
        std::scoped_lock lock(mutex_);
        return endpoints_.size();
    }

    std::shared_ptr<ITransportEndpoint> getEndpoint(TransportSessionId id) const {
        std::scoped_lock lock(mutex_);
        auto it = endpoints_.find(id);
        if (it == endpoints_.end()) {
            return nullptr;
        }
        return it->second;
    }

    bool hasEndpoint(TransportSessionId id) const {
        std::scoped_lock lock(mutex_);
        return endpoints_.find(id) != endpoints_.end();
    }

    mini::net::EventLoop* ownerLoop() const noexcept {
        return ownerLoop_;
    }

private:
    using EndpointMap = std::unordered_map<TransportSessionId, std::shared_ptr<ITransportEndpoint>>;

    template <typename Fn>
    void post(Fn&& fn) {
        if (!ownerLoop_) {
            return;
        }
        if (ownerLoop_->isInLoopThread()) {
            fn();
            return;
        }
        std::weak_ptr<void> lifetime = lifetimeToken_;
        ownerLoop_->queueInLoop([lifetime, fn = std::forward<Fn>(fn)]() mutable {
            if (!lifetime.lock()) {
                return;
            }
            fn();
        });
    }

    std::shared_ptr<ITransportEndpoint> getEndpointLocked(TransportSessionId id) const {
        std::scoped_lock lock(mutex_);
        auto it = endpoints_.find(id);
        if (it == endpoints_.end()) {
            return nullptr;
        }
        return it->second;
    }

    mini::net::EventLoop* ownerLoop_{nullptr};
    std::shared_ptr<void> lifetimeToken_;
    mutable std::mutex mutex_;
    EndpointMap endpoints_;
    std::atomic<TransportSessionId> nextSessionId_;
};

}  // namespace mini::net::transport
