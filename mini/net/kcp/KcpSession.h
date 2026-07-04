#pragma once

// KcpSession — 将 ITransportEndpoint 语义绑定到单个 KCP 会话。
//
// 它并不直接持有 UdpSocket，避免与 reactor loop 的强绑定；
// 仅观察 owner transport 与对端地址。owner 停止或移除会话时必须显式失效该观察指针。

#include "mini/net/transport/ITransport.h"
#include "mini/net/InetAddress.h"

#include <any>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace mini::net {
class EventLoop;
namespace kcp {
class KcpTransport;
}
}

namespace mini::net::kcp {

class KcpSession : public mini::net::transport::ITransportEndpoint {
public:
    using Ptr = std::shared_ptr<KcpSession>;

    KcpSession(KcpTransport* ownerTransport,
               mini::net::transport::TransportSessionId id,
               mini::net::InetAddress peerAddress);

    void send(std::string_view data) override;
    void shutdown() override;
    void forceClose() override;
    bool connected() const noexcept override;
    mini::net::EventLoop* getLoop() const noexcept override;
    std::string_view name() const noexcept override;

    mini::net::transport::TransportSessionId sessionId() const noexcept override;
    void setSessionId(mini::net::transport::TransportSessionId id) override;
    mini::net::transport::TransportKind transportKind() const noexcept override;
    void setTransportContext(std::any ctx) override;
    const std::any& getTransportContext() const noexcept override;
    std::any& getTransportContext() noexcept override;

    const mini::net::InetAddress& peerAddress() const noexcept;
    void markClosed() noexcept;
    bool hasOwner() const noexcept;

private:
    std::atomic<KcpTransport*> owner_;
    mini::net::transport::TransportSessionId sessionId_{mini::net::transport::kInvalidTransportSessionId};
    mini::net::transport::TransportKind transportKind_{mini::net::transport::TransportKind::kKcp};
    mini::net::InetAddress peerAddress_;
    std::string name_;
    std::atomic<bool> connected_{true};
    std::any transportContext_;
};

}  // namespace mini::net::kcp
