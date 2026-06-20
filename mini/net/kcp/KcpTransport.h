#pragma once

// KcpTransport — KCP 后端的轻量传输调度器（最小实现）。
//
// 目标：
// - 复用现有 EventLoop 模型，基于 UDP socket 接收/发送包；
// - 为每个对端维护一个 KcpSession，对外提供统一的 transport endpoint 能力；
// - 通过 runEvery 在 owner loop 上驱动周期 tick（用于重传）。

#include "mini/base/noncopyable.h"
#include "mini/net/kcp/KcpCodec.h"
#include "mini/net/transport/TransportTypes.h"
#include "mini/net/TimerId.h"
#include "mini/net/udp/UdpSocket.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mini::net {
class EventLoop;
class InetAddress;

namespace kcp {
class KcpSession;

class KcpTransport final : private mini::base::noncopyable {
public:
    using MessageCallback =
        std::function<void(transport::TransportSessionId sessionId, std::string_view packet, const InetAddress& peerAddr)>;
    using ErrorCallback = std::function<void(int errorCode)>;

    KcpTransport(EventLoop* loop,
                 const InetAddress& bindAddr,
                 std::string name = "kcp-transport",
                 bool reusePort = true);
    ~KcpTransport();

    void setMessageCallback(MessageCallback cb);
    void setErrorCallback(ErrorCallback cb);

    void start();
    void stop();
    bool started() const noexcept;

    std::shared_ptr<KcpSession> openSession(const InetAddress& peerAddr,
                                           transport::TransportSessionId preferredSessionId = transport::kInvalidTransportSessionId);

    std::shared_ptr<KcpSession> getSession(transport::TransportSessionId sessionId) const;
    void closeSession(transport::TransportSessionId sessionId);
    std::size_t sessionCount() const;

    void sendTo(transport::TransportSessionId sessionId, std::string_view data);
    void sendTo(const InetAddress& peerAddr, std::string_view data);

    EventLoop* getLoop() const noexcept;
    std::string_view name() const noexcept;

private:
    using SessionAddressMap = std::unordered_map<std::string, transport::TransportSessionId>;
    using SessionMap = std::unordered_map<transport::TransportSessionId, std::shared_ptr<KcpSession>>;

    static constexpr auto kFlushInterval = std::chrono::milliseconds(10);
    static constexpr auto kInitialRto = std::chrono::milliseconds(25);
    static constexpr auto kMaxRto = std::chrono::milliseconds(500);
    static constexpr std::size_t kMaxRetransmissions = 4;

    struct OutboundPacket {
        std::uint32_t seq{0};
        std::chrono::steady_clock::time_point lastSendAt;
        std::chrono::milliseconds rto{kInitialRto};
        std::size_t retryCount{0};
        std::string wirePacket;
    };

    struct SessionFlowState {
        std::uint32_t nextSendSeq{1};
        std::uint32_t nextRecvSeq{1};
        std::uint32_t lastRecvSeq{0};
        std::unordered_map<std::uint32_t, std::string> pendingPackets;
        std::unordered_map<std::uint32_t, OutboundPacket> inFlight;
    };

    static std::string makeAddressKey(const InetAddress& addr);
    void onPacket(std::string_view packet, const InetAddress& peerAddr);
    void removeSession(transport::TransportSessionId sessionId);
    void removeSessionByAddress(const InetAddress& peerAddr);
    std::shared_ptr<KcpSession> createOrGetSession(
        const InetAddress& peerAddr,
        transport::TransportSessionId preferredSessionId = transport::kInvalidTransportSessionId);
    std::shared_ptr<KcpSession> getSessionLocked(transport::TransportSessionId sessionId) const;

    SessionFlowState* getSessionStateLocked(transport::TransportSessionId sessionId);
    const SessionFlowState* getSessionStateLocked(transport::TransportSessionId sessionId) const;
    void removeSessionStateLocked(transport::TransportSessionId sessionId);

    void sendWirePacket(const std::shared_ptr<KcpSession>& session, std::string_view packet);
    void sendAck(const std::shared_ptr<KcpSession>& session, std::uint32_t ackSeq);
    void applyAck(SessionFlowState& state, std::uint32_t ackSeq);
    void processDataPayload(SessionFlowState& state,
                           const codec::KcpFrame& frame,
                           std::vector<std::string>& deliverPayloads);

    template <typename Fn>
    void post(Fn&& fn);

    void startFlushTimer();
    void stopFlushTimer();
    void handleFlushTick();

    EventLoop* loop_;
    std::string name_;
    std::unique_ptr<udp::UdpSocket> socket_;
    MessageCallback messageCallback_;
    ErrorCallback errorCallback_;
    bool started_{false};
    transport::TransportSessionId nextSessionId_{transport::kFirstTransportSessionId};
    TimerId flushTimerId_;
    bool hasFlushTimer_{false};

    SessionAddressMap sessionByAddr_;
    SessionMap sessions_;
    std::unordered_map<transport::TransportSessionId, SessionFlowState> sessionStates_;

    mutable std::mutex mutex_;
};

}  // namespace kcp

}  // namespace mini::net
