#pragma once

// UdpTransportEndpoint — 将 UdpServer 的单个远端会话暴露为 ITransportEndpoint。
//
// 它不拥有 UdpServer；通过 lifetime token 观察 server 是否仍有效。
// send/close 仍委托给 UdpServer，由 UdpServer 回到自己的 owner loop 执行。

#include "mini/net/transport/ITransport.h"
#include "mini/net/udp/UdpServer.h"

#include <any>
#include <memory>
#include <string>
#include <string_view>

namespace mini::net::transport {

class UdpTransportEndpoint final : public ITransportEndpoint {
public:
    UdpTransportEndpoint(udp::UdpServer* owner,
                         std::weak_ptr<void> lifetime,
                         TransportSessionId sessionId)
        : owner_(owner),
          lifetime_(std::move(lifetime)),
          sessionId_(sessionId),
          name_("udp-session-" + std::to_string(sessionId)) {
    }

    void send(std::string_view data) override {
        if (!alive() || owner_ == nullptr) {
            return;
        }
        owner_->sendTo(sessionId_, data);
    }

    void shutdown() override {
        forceClose();
    }

    void forceClose() override {
        if (!alive() || owner_ == nullptr) {
            return;
        }
        owner_->closeSession(sessionId_);
    }

    bool connected() const noexcept override {
        return alive() && owner_ != nullptr && owner_->hasSession(sessionId_);
    }

    mini::net::EventLoop* getLoop() const noexcept override {
        if (!alive() || owner_ == nullptr) {
            return nullptr;
        }
        return owner_->getLoop();
    }

    std::string_view name() const noexcept override {
        return name_;
    }

    TransportSessionId sessionId() const noexcept override {
        return sessionId_;
    }

    void setSessionId(TransportSessionId id) override {
        sessionId_ = id;
        name_ = "udp-session-" + std::to_string(id);
    }

    TransportKind transportKind() const noexcept override {
        return TransportKind::kUdp;
    }

    void setTransportContext(std::any ctx) override {
        transportContext_ = std::move(ctx);
    }

    const std::any& getTransportContext() const noexcept override {
        return transportContext_;
    }

    std::any& getTransportContext() noexcept override {
        return transportContext_;
    }

private:
    bool alive() const noexcept {
        return !lifetime_.expired();
    }

    udp::UdpServer* owner_{nullptr};
    std::weak_ptr<void> lifetime_;
    TransportSessionId sessionId_{kInvalidTransportSessionId};
    std::string name_;
    std::any transportContext_;
};

}  // namespace mini::net::transport
