#pragma once

// ITransport — 连接会话层和通道层的最小抽象。
//
// ITransportChannel 只关心可执行的 I/O 操作；
// ITransportSession 关心会话标识与可携带状态（用于未来异构 Session 管理）。
//
// 该抽象不要求感知 TLS/UDP/KCP 细节，只保留上层协议需要的统一行为。

#include "mini/net/transport/TransportTypes.h"

#include <any>
#include <functional>
#include <memory>
#include <string_view>

namespace mini::net {
class Buffer;
class EventLoop;
}

namespace mini::net::transport {

// 前置声明，避免在回调类型上形成强依赖。
class ITransportChannel;
class ITransportSession;

// Transport 通道回调：上层 session 层在通道上感知的事件。
using TransportReadCallback = std::function<void(ITransportChannel& channel, mini::net::Buffer* buffer)>;
using TransportCloseCallback = std::function<void(ITransportSession& session)>;

// ────────────────────────────────────────────────────────────────────────────
// ITransportChannel
// ────────────────────────────────────────────────────────────────────────────

class ITransportChannel {
public:
    virtual ~ITransportChannel() = default;

    virtual void send(std::string_view data) = 0;
    virtual void shutdown() = 0;
    virtual void forceClose() = 0;
    virtual bool connected() const noexcept = 0;
    virtual mini::net::EventLoop* getLoop() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;
};

// ────────────────────────────────────────────────────────────────────────────
// ITransportSession
// ────────────────────────────────────────────────────────────────────────────

class ITransportSession {
public:
    virtual ~ITransportSession() = default;

    virtual TransportSessionId sessionId() const noexcept = 0;
    virtual void setSessionId(TransportSessionId id) = 0;
    virtual TransportKind transportKind() const noexcept = 0;
    virtual void setTransportContext(std::any ctx) = 0;
    virtual const std::any& getTransportContext() const noexcept = 0;
    virtual std::any& getTransportContext() noexcept = 0;
};

// ----------------------------------------------------------------------------
// 统一 endpoint 视图：同时具有 I/O 通道与会话语义，可直接被 Manager 管理。
// ----------------------------------------------------------------------------
class ITransportEndpoint : public ITransportChannel, public ITransportSession {
public:
    ~ITransportEndpoint() override = default;
};

}  // namespace mini::net::transport
