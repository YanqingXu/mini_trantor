#include "mini/net/ProtocolConnectionAdapter.h"

#include "mini/net/TcpConnection.h"

#include <cassert>
#include <utility>

namespace mini::net {

ProtocolConnectionAdapter::ProtocolConnectionAdapter(const TcpConnectionPtr& conn)
    : conn_(conn),
      name_(conn->name()) {
}

void ProtocolConnectionAdapter::send(std::string_view data) {
    if (auto c = conn_.lock()) {
        c->send(data);
    }
}

void ProtocolConnectionAdapter::shutdown() {
    if (auto c = conn_.lock()) {
        c->shutdown();
    }
}

void ProtocolConnectionAdapter::forceClose() {
    if (auto c = conn_.lock()) {
        c->forceClose();
    }
}

bool ProtocolConnectionAdapter::connected() const noexcept {
    if (auto c = conn_.lock()) {
        return c->connected();
    }
    return false;
}

EventLoop* ProtocolConnectionAdapter::getLoop() const noexcept {
    if (auto c = conn_.lock()) {
        return c->getLoop();
    }
    return nullptr;
}

std::string_view ProtocolConnectionAdapter::name() const noexcept {
    return name_;
}

void ProtocolConnectionAdapter::setProtocolContext(std::any ctx) {
    protocolContext_ = std::move(ctx);
}

const std::any& ProtocolConnectionAdapter::getProtocolContext() const noexcept {
    return protocolContext_;
}

std::any& ProtocolConnectionAdapter::getProtocolContext() noexcept {
    return protocolContext_;
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

std::shared_ptr<ProtocolConnectionAdapter>
ProtocolConnectionAdapter::createAndBind(const TcpConnectionPtr& conn) {
    auto adapter = std::make_shared<ProtocolConnectionAdapter>(conn);
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
