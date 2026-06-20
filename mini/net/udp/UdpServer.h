#pragma once

// UdpServer 是 UDP 面向会话的轻量服务端：
// - 在单一 EventLoop 上接收 UDP datagram
// - 以远端地址维护 sessionId（与 TransportSessionId 兼容）
// - 提供 session 粒度和地址粒度发送接口

#include "mini/base/noncopyable.h"
#include "mini/net/transport/TransportTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mini::net {

class EventLoop;
class InetAddress;
namespace transport {
class ITransportEndpoint;
}

namespace udp {

class UdpSocket;

class UdpServer : private mini::base::noncopyable {
public:
    using MessageCallback = std::function<void(transport::TransportSessionId sessionId,
                                              std::string_view packet,
                                              const InetAddress& peerAddr)>;
    using ErrorCallback = std::function<void(int errorCode)>;

    UdpServer(EventLoop* loop, const InetAddress& listenAddr, std::string name = "udp-server", bool reusePort = true);
    ~UdpServer();

    void setMessageCallback(MessageCallback cb);
    void setErrorCallback(ErrorCallback cb);

    void start();
    void stop();

    void sendTo(transport::TransportSessionId sessionId, std::string_view data);
    void sendTo(const InetAddress& peerAddr, std::string_view data);
    void closeSession(transport::TransportSessionId sessionId);
    std::shared_ptr<transport::ITransportEndpoint> getTransportEndpoint(transport::TransportSessionId sessionId) const;

    bool started() const noexcept;
    std::size_t sessionCount() const;
    bool hasSession(transport::TransportSessionId sessionId) const;
    EventLoop* getLoop() const noexcept;
    std::string_view name() const noexcept;

private:
    void onPacket(std::string_view packet, const InetAddress& peerAddr);
    void removeSession(transport::TransportSessionId sessionId);

    template <typename Fn>
    void post(Fn&& fn);

    transport::TransportSessionId nextSessionId();

    using SessionAddressMap = std::unordered_map<std::string, transport::TransportSessionId>;
    using SessionPeerMap = std::unordered_map<transport::TransportSessionId, InetAddress>;

    EventLoop* loop_;
    std::string name_;
    std::unique_ptr<UdpSocket> socket_;
    std::shared_ptr<void> lifetimeToken_;
    MessageCallback messageCallback_;
    ErrorCallback errorCallback_;
    bool started_{false};
    std::uint64_t nextSessionId_{transport::kFirstTransportSessionId};
    SessionAddressMap sessionByAddr_;
    SessionPeerMap peerBySession_;
};

}  // namespace udp

}  // namespace mini::net
