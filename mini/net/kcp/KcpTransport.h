#pragma once

// KcpTransport — KCP-style reliable UDP preview backend.
//
// 目标：
// - 复用现有 EventLoop 模型，基于 UDP socket 接收/发送包；
// - 为每个对端维护一个 KcpSession，对外提供统一的 transport endpoint 能力；
// - 通过 runEvery 在 owner loop 上驱动周期 tick（用于重传）。
// - 保持为 transport preview：高级 PMTU/FEC/congestion 能力必须显式
//   通过 options 开启，不能被描述为生产级 KCP 协议栈。

#include "mini/base/noncopyable.h"
#include "mini/net/kcp/KcpCodec.h"
#include "mini/net/transport/PathMtuCache.h"
#include "mini/net/transport/TransportTypes.h"
#include "mini/net/TimerId.h"
#include "mini/net/udp/UdpSocket.h"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mini::net {
class EventLoop;
class InetAddress;

namespace kcp {
class KcpSession;

struct KcpTransportOptions {
    std::chrono::milliseconds initialRto{std::chrono::milliseconds(25)};
    std::chrono::milliseconds maxRto{std::chrono::milliseconds(500)};
    std::size_t maxRetransmissions{4};
    std::size_t maxDatagramPayloadSize{1200};
    std::size_t maxApplicationPayloadSize{256 * 1024};
    bool enableMtuProbing{false};
    std::size_t minDatagramPayloadSize{1200};
    std::size_t mtuProbeStepBytes{200};
    std::size_t mtuProbeMaxRetries{1};
    std::chrono::milliseconds mtuProbeInterval{std::chrono::milliseconds(100)};
    std::chrono::milliseconds mtuProbeBlackholeCooldown{std::chrono::milliseconds(1000)};
    bool enableMtuPathCache{false};
    std::shared_ptr<transport::PathMtuCache> sharedMtuPathCache{};
    bool enablePlatformPathMtuSignals{false};
    bool enableRawIcmpPathMtuSignals{false};
    bool enablePathMtuSignalAuthentication{false};
    bool enableCongestionWindow{false};
    std::size_t minCongestionWindow{1};
    std::size_t initialCongestionWindow{32};
    std::size_t maxCongestionWindow{256};
    bool enableRedundantCopies{false};
    std::size_t redundantCopyCount{1};
    bool enableXorParityRecovery{false};
    std::size_t xorParityGroupSize{4};
};

class KcpTransport final : private mini::base::noncopyable {
public:
    using MessageCallback =
        std::function<void(transport::TransportSessionId sessionId, std::string_view packet, const InetAddress& peerAddr)>;
    using ErrorCallback = std::function<void(int errorCode)>;

    KcpTransport(EventLoop* loop,
                 const InetAddress& bindAddr,
                 std::string name = "kcp-transport",
                 bool reusePort = true,
                 KcpTransportOptions options = {});
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
    void notifyPathMtuFailure(const InetAddress& peerAddr,
                              std::size_t failedDatagramPayloadSize,
                              std::size_t suggestedDatagramPayloadSize = 0);
    void notifyPathMtuFailure(const udp::PathMtuFailure& failure);

    EventLoop* getLoop() const noexcept;
    std::string_view name() const noexcept;
    const KcpTransportOptions& options() const noexcept;

    static constexpr std::size_t kDefaultMtuBytes = 1200;
    static constexpr std::size_t kMaxSingleFramePayloadSize =
        kDefaultMtuBytes - codec::kKcpFrameHeaderSize;
    static constexpr std::size_t kFragmentHeaderSize = 8;
    static constexpr std::size_t kMaxFragmentPayloadSize =
        kDefaultMtuBytes - codec::kKcpFrameHeaderSize - kFragmentHeaderSize;
    static constexpr std::size_t kMaxApplicationPayloadSize = 256 * 1024;

private:
    using SessionAddressMap = std::unordered_map<std::string, transport::TransportSessionId>;
    using SessionMap = std::unordered_map<transport::TransportSessionId, std::shared_ptr<KcpSession>>;

    static constexpr auto kFlushInterval = std::chrono::milliseconds(10);
    static constexpr auto kDefaultInitialRto = std::chrono::milliseconds(25);
    static constexpr auto kDefaultMaxRto = std::chrono::milliseconds(500);
    static constexpr std::size_t kDefaultMaxRetransmissions = 4;
    static constexpr std::size_t kMaxSelectiveAckEntries = 32;
    static constexpr std::size_t kMaxRedundantCopyCount = 3;
    static constexpr std::size_t kDefaultXorParityGroupSize = 4;
    static constexpr std::size_t kMaxXorParityGroupSize = 16;
    static constexpr std::size_t kMaxXorParityHistoryPackets = 128;

    struct OutboundPacket {
        transport::TransportSessionId sessionId{transport::kInvalidTransportSessionId};
        std::uint32_t seq{0};
        std::uint16_t flags{codec::kKcpFrameFlagNone};
        std::chrono::steady_clock::time_point lastSendAt;
        std::chrono::milliseconds rto{kDefaultInitialRto};
        std::size_t retryCount{0};
        std::string payload;
        std::string wirePacket;
    };

    struct PendingPacket {
        std::uint16_t flags{codec::kKcpFrameFlagNone};
        std::string payload;
    };

    struct FragmentAssembly {
        std::uint16_t fragmentCount{0};
        std::vector<std::string> fragments;
        std::vector<bool> received;
        std::size_t receivedCount{0};
        std::size_t bytes{0};
    };

    struct ParityPacket {
        std::uint32_t seq{0};
        std::uint16_t flags{codec::kKcpFrameFlagNone};
        std::string payload;
    };

    struct XorParityPayload {
        std::uint32_t baseSeq{0};
        std::vector<std::uint16_t> flags;
        std::vector<std::uint16_t> payloadSizes;
        std::string parityPayload;
    };

    struct SessionFlowState {
        std::uint32_t nextSendSeq{1};
        std::uint32_t nextRecvSeq{1};
        std::uint32_t lastRecvSeq{0};
        std::uint32_t nextMessageId{1};
        std::size_t currentDatagramPayloadSize{0};
        std::size_t congestionWindow{0};
        std::size_t congestionSlowStartThreshold{0};
        std::size_t congestionAckCount{0};
        std::size_t mtuProbeTargetDatagramPayloadSize{0};
        std::size_t mtuProbeRetryCount{0};
        std::chrono::steady_clock::time_point lastMtuProbeAt{};
        std::chrono::steady_clock::time_point mtuProbeCooldownUntil{};
        std::size_t mtuProbeBlackholeCount{0};
        std::string mtuProbeWirePacket;
        bool mtuProbeInFlight{false};
        bool mtuProbeDisabled{false};
        std::vector<ParityPacket> xorParitySendGroup;
        std::unordered_map<std::uint32_t, PendingPacket> xorParityHistory;
        std::unordered_map<std::uint32_t, PendingPacket> pendingPackets;
        std::unordered_map<std::uint32_t, OutboundPacket> inFlight;
        std::deque<OutboundPacket> sendQueue;
        std::unordered_map<std::uint32_t, FragmentAssembly> fragmentAssemblies;
    };

    static KcpTransportOptions normalizeOptions(KcpTransportOptions options) noexcept;
    static std::string makeAddressKey(const InetAddress& addr);
    std::size_t initialDatagramPayloadSize() const noexcept;
    std::size_t effectiveDatagramPayloadSize(SessionFlowState& state) const noexcept;
    void seedMtuStateFromPathCacheLocked(SessionFlowState& state,
                                         const InetAddress& peerAddress,
                                         std::chrono::steady_clock::time_point now);
    void recordMtuPathSuccessLocked(const InetAddress& peerAddress,
                                    std::size_t confirmedDatagramPayloadSize);
    void recordMtuPathBlackholeLocked(const InetAddress& peerAddress,
                                      const SessionFlowState& state);
    void recordMtuPathFailureLocked(const InetAddress& peerAddress,
                                    const SessionFlowState& state,
                                    std::size_t safeDatagramPayloadSize);
    std::optional<transport::PathMtuCacheEntry> findMtuPathCacheEntryLocked(
        const InetAddress& peerAddress);
    void clearMtuPathCacheCooldownLocked(const InetAddress& peerAddress);
    static std::optional<transport::TransportSessionId> decodeQuotedKcpSessionId(
        std::string_view quotedUdpPayloadPrefix);
    bool authenticatePathMtuFailureLocked(const udp::PathMtuFailure& failure,
                                          transport::TransportSessionId sessionId) const;
    void applyPathMtuFailureSignal(udp::PathMtuFailure failure, bool authenticate);
    std::size_t safeDatagramPayloadSizeAfterMtuFailure(
        std::size_t failedDatagramPayloadSize,
        std::size_t suggestedDatagramPayloadSize) const noexcept;
    void applyPathMtuFailureLocked(SessionFlowState& state,
                                   const InetAddress& peerAddress,
                                   std::size_t failedDatagramPayloadSize,
                                   std::size_t suggestedDatagramPayloadSize,
                                   std::chrono::steady_clock::time_point now);
    static std::size_t singleFramePayloadLimit(std::size_t datagramPayloadSize) noexcept;
    static std::size_t fragmentPayloadLimit(std::size_t datagramPayloadSize) noexcept;
    std::size_t effectiveCongestionWindow(SessionFlowState& state) const noexcept;
    void onPacketsAckedLocked(SessionFlowState& state, std::size_t ackedPackets) const noexcept;
    void onRetransmissionTimeoutLocked(SessionFlowState& state) const noexcept;
    void queueOrSendPacketLocked(SessionFlowState& state,
                                 OutboundPacket packet,
                                 std::chrono::steady_clock::time_point now,
                                 std::vector<std::string>& toSend);
    void appendInitialWireCopies(const OutboundPacket& packet,
                                 std::vector<std::string>& toSend) const;
    void appendXorParityIfReadyLocked(SessionFlowState& state,
                                      const OutboundPacket& packet,
                                      std::vector<std::string>& toSend);
    void drainSendQueueLocked(SessionFlowState& state,
                              std::chrono::steady_clock::time_point now,
                              std::vector<std::string>& toSend);
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
    void sendAck(const std::shared_ptr<KcpSession>& session, std::uint32_t ackSeq, std::string selectiveAckPayload);
    void sendMtuProbeAck(const std::shared_ptr<KcpSession>& session, std::uint32_t ackSeq, std::string ackPayload);
    std::size_t applyAck(SessionFlowState& state,
                         std::uint32_t ackSeq,
                         std::uint16_t flags,
                         std::string_view ackPayload);
    void applyMtuProbeAck(SessionFlowState& state,
                          const InetAddress& peerAddress,
                          std::string_view ackPayload);
    void processDataPayload(SessionFlowState& state,
                           const codec::KcpFrame& frame,
                           std::vector<std::string>& deliverPayloads);
    bool processXorParityPayload(SessionFlowState& state,
                                 std::string_view payload,
                                 std::vector<std::string>& deliverPayloads);
    void processReliablePayload(SessionFlowState& state,
                                std::uint16_t flags,
                                std::string payload,
                                std::vector<std::string>& deliverPayloads);
    void processFragmentPayload(SessionFlowState& state,
                                std::string payload,
                                std::vector<std::string>& deliverPayloads);

    static std::string encodeFragmentPayload(std::uint32_t messageId,
                                             std::uint16_t fragmentIndex,
                                             std::uint16_t fragmentCount,
                                             std::string_view payload,
                                             std::size_t maxFragmentPayloadSize);
    static bool decodeFragmentPayload(std::string_view payload,
                                      std::uint32_t& messageId,
                                      std::uint16_t& fragmentIndex,
                                      std::uint16_t& fragmentCount,
                                      std::string_view& fragmentPayload);
    static std::string encodeSelectiveAckPayload(const SessionFlowState& state);
    static std::vector<std::uint32_t> decodeSelectiveAckPayload(std::string_view payload);
    void rememberXorParityPacketLocked(SessionFlowState& state,
                                       std::uint32_t seq,
                                       std::uint16_t flags,
                                       const std::string& payload) const;
    static std::string encodeXorParityPayload(const std::vector<ParityPacket>& packets);
    static bool decodeXorParityPayload(std::string_view payload, XorParityPayload& out);
    static std::string encodeMtuProbePayload(std::size_t targetDatagramPayloadSize);
    static std::string encodeMtuProbeAckPayload(std::size_t targetDatagramPayloadSize);
    static bool decodeMtuProbePayload(std::string_view payload, std::size_t& targetDatagramPayloadSize);
    static bool decodeMtuProbeAckPayload(std::string_view payload, std::size_t& targetDatagramPayloadSize);
    static std::string makeMtuProbeFramePayload(std::size_t targetDatagramPayloadSize);
    void maybeQueueMtuProbeLocked(transport::TransportSessionId sessionId,
                                  SessionFlowState& state,
                                  const InetAddress& peerAddress,
                                  std::chrono::steady_clock::time_point now,
                                  std::vector<std::pair<std::string, InetAddress>>& toSend);

    template <typename Fn>
    void post(Fn&& fn);

    void startFlushTimer();
    void stopFlushTimer();
    void handleFlushTick();
    void stopInLoop();

    EventLoop* loop_;
    std::string name_;
    KcpTransportOptions options_;
    std::unique_ptr<udp::UdpSocket> socket_;
    std::shared_ptr<void> lifetimeToken_;
    MessageCallback messageCallback_;
    ErrorCallback errorCallback_;
    std::atomic<bool> started_{false};
    transport::TransportSessionId nextSessionId_{transport::kFirstTransportSessionId};
    TimerId flushTimerId_;
    bool hasFlushTimer_{false};

    SessionAddressMap sessionByAddr_;
    SessionMap sessions_;
    std::unordered_map<transport::TransportSessionId, SessionFlowState> sessionStates_;
    std::unordered_map<std::string, transport::PathMtuCacheEntry> mtuPathCache_;

    mutable std::mutex mutex_;
};

}  // namespace kcp

}  // namespace mini::net
