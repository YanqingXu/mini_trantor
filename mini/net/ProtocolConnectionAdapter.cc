#include "mini/net/ProtocolConnectionAdapter.h"

#include "mini/net/TcpConnection.h"
#include "mini/net/transport/TransportEndpoint.h"

#include <utility>

namespace mini::net {

ProtocolConnectionAdapter::ProtocolConnectionAdapter(std::shared_ptr<transport::ITransportChannel> channel,
                                                   std::shared_ptr<transport::ITransportSession> session)
    : channel_(std::move(channel)),
      session_(std::move(session)),
      name_(std::string{}) {
    if (auto locked = channel_.lock()) {
        name_ = locked->name();
    }
}

void ProtocolConnectionAdapter::send(std::string_view data) {
    if (auto c = channel_.lock()) {
        c->send(data);
    }
}

void ProtocolConnectionAdapter::shutdown() {
    if (auto c = channel_.lock()) {
        c->shutdown();
    }
}

void ProtocolConnectionAdapter::forceClose() {
    if (auto c = channel_.lock()) {
        c->forceClose();
    }
}

bool ProtocolConnectionAdapter::connected() const noexcept {
    if (auto c = channel_.lock()) {
        return c->connected();
    }
    return false;
}

EventLoop* ProtocolConnectionAdapter::getLoop() const noexcept {
    if (auto c = channel_.lock()) {
        return c->getLoop();
    }
    return nullptr;
}

std::string_view ProtocolConnectionAdapter::name() const noexcept {
    return name_;
}

transport::TransportSessionId
ProtocolConnectionAdapter::sessionId() const noexcept {
    if (session_) {
        return session_->sessionId();
    }
    return transport::kInvalidTransportSessionId;
}

void ProtocolConnectionAdapter::setSessionId(
    transport::TransportSessionId id) {
    if (session_) {
        session_->setSessionId(id);
    }
}

transport::TransportKind
ProtocolConnectionAdapter::transportKind() const noexcept {
    if (session_) {
        return session_->transportKind();
    }
    return transport::TransportKind::kUnknown;
}

void ProtocolConnectionAdapter::setProtocolContext(std::any ctx) {
    protocolContext_ = std::move(ctx);
    if (session_) {
        session_->setTransportContext(protocolContext_);
    }
}

const std::any& ProtocolConnectionAdapter::getProtocolContext() const noexcept {
    if (session_) {
        return session_->getTransportContext();
    }
    return protocolContext_;
}

std::any& ProtocolConnectionAdapter::getProtocolContext() noexcept {
    if (session_) {
        return session_->getTransportContext();
    }
    return protocolContext_;
}

void ProtocolConnectionAdapter::setTransportContext(std::any ctx) {
    if (session_) {
        session_->setTransportContext(std::move(ctx));
    }
}

const std::any& ProtocolConnectionAdapter::getTransportContext() const noexcept {
    if (session_) {
        return session_->getTransportContext();
    }
    return protocolContext_;
}

std::any& ProtocolConnectionAdapter::getTransportContext() noexcept {
    if (session_) {
        return session_->getTransportContext();
    }
    return protocolContext_;
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

std::shared_ptr<ProtocolConnectionAdapter>
ProtocolConnectionAdapter::createAndBind(const TcpConnectionPtr& conn) {
    auto endpoint = transport::TransportEndpoint::create(conn);
    auto adapter = std::make_shared<ProtocolConnectionAdapter>(endpoint, endpoint);
    conn->setContext(adapter);
    return adapter;
}

ProtocolConnectionAdapter*
ProtocolConnectionAdapter::getFrom(const TcpConnectionPtr& conn) {
    auto* sp = std::any_cast<std::shared_ptr<ProtocolConnectionAdapter>>(
        &conn->getContext());
    if (!sp) return nullptr;
    return sp->get();
}

std::shared_ptr<ProtocolConnectionAdapter>
ProtocolConnectionAdapter::sharedFrom(const TcpConnectionPtr& conn) {
    auto* sp = std::any_cast<std::shared_ptr<ProtocolConnectionAdapter>>(
        &conn->getContext());
    if (!sp) return nullptr;
    return *sp;
}

}  // namespace mini::net
