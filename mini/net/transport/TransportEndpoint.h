#pragma once

// TransportEndpoint — 统一接入传输端点的薄包装层。
//
// 当前阶段它以 TcpConnection 为底层状态机载体，但对外以 ITransport* 接口暴露；
// 其核心目标是：上层 Session/Protocol 不再直接感知 TCP/TLS 细节。

#include "mini/net/transport/ITransport.h"
#include "mini/net/TcpConnection.h"

#include <any>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace mini::net {
class TcpConnection;
class EventLoop;
class Buffer;
}

namespace mini::net::transport {

class TransportEndpoint final : public ITransportEndpoint {
public:
    using Ptr = std::shared_ptr<TransportEndpoint>;

    static Ptr create(const std::shared_ptr<::mini::net::TcpConnection>& conn,
                     TransportSessionId id = kInvalidTransportSessionId,
                     TransportKind kind = TransportKind::kTcp);

    TransportEndpoint(std::shared_ptr<::mini::net::TcpConnection> conn,
                     TransportSessionId id,
                     TransportKind kind);

    // ITransportChannel
    void send(std::string_view data) override;
    void shutdown() override;
    void forceClose() override;
    bool connected() const noexcept override;
    ::mini::net::EventLoop* getLoop() const noexcept override;
    std::string_view name() const noexcept override;

    // ITransportSession
    TransportSessionId sessionId() const noexcept override;
    void setSessionId(TransportSessionId id) override;
    TransportKind transportKind() const noexcept override;
    void setTransportContext(std::any ctx) override;
    const std::any& getTransportContext() const noexcept override;
    std::any& getTransportContext() noexcept override;

private:
    std::weak_ptr<::mini::net::TcpConnection> conn_;
    TransportSessionId sessionId_{kInvalidTransportSessionId};
    TransportKind kind_{TransportKind::kUnknown};
    std::string name_;
    std::any transportContext_;
};

// ────────────────────────────────────────────────────────────────────────────
// Inline definitions
// ────────────────────────────────────────────────────────────────────────────

inline TransportEndpoint::TransportEndpoint(std::shared_ptr<::mini::net::TcpConnection> conn,
                                         TransportSessionId id,
                                         TransportKind kind)
    : conn_(std::move(conn)),
      sessionId_(id),
      kind_(kind),
      name_(std::string{}) {
    if (auto locked = conn_.lock()) {
        name_ = locked->name();
    }
}

inline TransportEndpoint::Ptr TransportEndpoint::create(const std::shared_ptr<::mini::net::TcpConnection>& conn,
                                                     TransportSessionId id,
                                                     TransportKind kind) {
    return std::make_shared<TransportEndpoint>(conn, id, kind);
}

inline void TransportEndpoint::send(std::string_view data) {
    if (auto conn = conn_.lock()) {
        conn->send(data);
    }
}

inline void TransportEndpoint::shutdown() {
    if (auto conn = conn_.lock()) {
        conn->shutdown();
    }
}

inline void TransportEndpoint::forceClose() {
    if (auto conn = conn_.lock()) {
        conn->forceClose();
    }
}

inline bool TransportEndpoint::connected() const noexcept {
    if (auto conn = conn_.lock()) {
        return conn->connected();
    }
    return false;
}

inline ::mini::net::EventLoop* TransportEndpoint::getLoop() const noexcept {
    if (auto conn = conn_.lock()) {
        return conn->getLoop();
    }
    return nullptr;
}

inline std::string_view TransportEndpoint::name() const noexcept {
    return name_;
}

inline TransportSessionId TransportEndpoint::sessionId() const noexcept {
    return sessionId_;
}

inline void TransportEndpoint::setSessionId(TransportSessionId id) {
    sessionId_ = id;
}

inline TransportKind TransportEndpoint::transportKind() const noexcept {
    return kind_;
}

inline void TransportEndpoint::setTransportContext(std::any ctx) {
    transportContext_ = std::move(ctx);
}

inline const std::any& TransportEndpoint::getTransportContext() const noexcept {
    return transportContext_;
}

inline std::any& TransportEndpoint::getTransportContext() noexcept {
    return transportContext_;
}

}  // namespace mini::net::transport
