#pragma once

// KcpSession — 将 ITransportEndpoint 语义绑定到单个 KCP 会话。
//
// 它并不直接持有 UdpSocket，避免与 reactor loop 的强绑定；
// 仅持有 owner transport 的非拥有指针与对端地址。这样可兼容栈/共享对象创建方式，
// 又方便统一交给 TransportManager 注册管理。

#include "mini/net/transport/ITransport.h"
#include "mini/net/InetAddress.h"

#include <any>
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
    KcpTransport* owner_;
    mini::net::transport::TransportSessionId sessionId_{mini::net::transport::kInvalidTransportSessionId};
    mini::net::transport::TransportKind transportKind_{mini::net::transport::TransportKind::kKcp};
    mini::net::InetAddress peerAddress_;
    std::string name_;
    bool connected_{true};
    std::any transportContext_;
};

}  // namespace mini::net::kcp
