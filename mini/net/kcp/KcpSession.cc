#include "mini/net/kcp/KcpSession.h"

#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/kcp/KcpTransport.h"

namespace mini::net::kcp {

KcpSession::KcpSession(KcpTransport* ownerTransport,
                       mini::net::transport::TransportSessionId id,
                       mini::net::InetAddress peerAddress)
    : owner_(ownerTransport),
      sessionId_(id),
      peerAddress_(std::move(peerAddress)),
      name_("kcp-session-" + std::to_string(id)) {}

mini::net::transport::TransportSessionId KcpSession::sessionId() const noexcept {
    return sessionId_;
}

void KcpSession::setSessionId(mini::net::transport::TransportSessionId id) {
    sessionId_ = id;
    name_ = "kcp-session-" + std::to_string(id);
}

mini::net::transport::TransportKind KcpSession::transportKind() const noexcept {
    return transportKind_;
}

void KcpSession::setTransportContext(std::any ctx) {
    transportContext_ = std::move(ctx);
}

const std::any& KcpSession::getTransportContext() const noexcept {
    return transportContext_;
}

std::any& KcpSession::getTransportContext() noexcept {
    return transportContext_;
}

const mini::net::InetAddress& KcpSession::peerAddress() const noexcept {
    return peerAddress_;
}

bool KcpSession::connected() const noexcept {
    return connected_.load(std::memory_order_acquire);
}

void KcpSession::send(std::string_view data) {
    auto* owner = owner_.load(std::memory_order_acquire);
    if (!owner || !connected_.load(std::memory_order_acquire)) {
        return;
    }
    owner->sendTo(sessionId_, data);
}

void KcpSession::shutdown() {
    forceClose();
}

void KcpSession::forceClose() {
    connected_.store(false, std::memory_order_release);
    if (auto* owner = owner_.load(std::memory_order_acquire)) {
        owner->closeSession(sessionId_);
    }
}

void KcpSession::markClosed() noexcept {
    connected_.store(false, std::memory_order_release);
    owner_.store(nullptr, std::memory_order_release);
}

bool KcpSession::hasOwner() const noexcept {
    return owner_.load(std::memory_order_acquire) != nullptr;
}

std::string_view KcpSession::name() const noexcept {
    return name_;
}

mini::net::EventLoop* KcpSession::getLoop() const noexcept {
    auto* owner = owner_.load(std::memory_order_acquire);
    if (!owner) {
        return nullptr;
    }
    return owner->getLoop();
}

}  // namespace mini::net::kcp
