#pragma once

// GameServerPipeline — 游戏服务器默认网络切片绑定器。
//
// 它不拥有 TcpServer / SessionManager / LogicLoop / TransportManager；
// 只负责把 TCP framed packet -> session auth -> logic command -> owner-loop send
// 这条最小默认路径接起来，供示例和集成测试复用。

#include "mini/base/MetricsHook.h"
#include "mini/base/noncopyable.h"
#include "mini/game/GameBackpressurePolicy.h"
#include "mini/game/GameGatewaySecurityPolicy.h"
#include "mini/base/Timestamp.h"
#include "mini/net/framing/PacketFramer.h"
#include "mini/net/transport/TransportTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mini::net {
class TcpServer;
class TcpConnection;
class Buffer;
namespace transport {
class ITransportEndpoint;
class TransportManager;
}
}

namespace mini::game {
class SessionManager;
namespace logic {
class LogicLoop;
}

class GameServerPipeline : private mini::base::noncopyable {
public:
    using AuthTokenValidator =
        std::function<bool(std::string_view sessionToken, std::string_view nonce)>;

    struct Options {
        std::uint32_t authMsgId{1};
        std::uint32_t commandMsgId{2};
        std::uint32_t broadcastMsgId{3};
        std::uint32_t responseMsgId{4};
        std::string defaultGroup{"room:default"};
        std::string defaultAoi{"aoi:default"};
        GameBackpressureOptions backpressure{};
        GameSecurityOptions security{};

        void validate() const {
            backpressure.validate();
            security.validate();
        }
    };

    GameServerPipeline(net::TcpServer& server,
                       net::transport::TransportManager& transportManager,
                       SessionManager& sessionManager,
                       logic::LogicLoop& logicLoop);
    GameServerPipeline(net::TcpServer& server,
                       net::transport::TransportManager& transportManager,
                       SessionManager& sessionManager,
                       logic::LogicLoop& logicLoop,
                       Options options);
    ~GameServerPipeline();

    void install();
    void setMetricCallback(GamePipelineMetricCallback callback);
    void setBackpressureMetricCallback(GameBackpressureMetricCallback callback);
    void setSecurityMetricCallback(GameSecurityMetricCallback callback);
    void setAuthTokenValidator(AuthTokenValidator validator);

    const Options& options() const noexcept;

private:
    struct ConnectionState {
        net::transport::TransportSessionId transportSessionId{
            net::transport::kInvalidTransportSessionId};
        std::shared_ptr<net::transport::ITransportEndpoint> endpoint;
        net::framing::PacketFramer framer;
        std::string input;
        std::string sessionToken;
        std::string authNonce;
        bool authenticated{false};
        bool processingInput{false};
        bool continuationScheduled{false};
    };

    struct AuthFrame {
        std::string sessionToken;
        std::string nonce;
    };

    struct SessionRateState {
        mini::base::Timestamp windowStartedAt{};
        mini::base::Timestamp lastSeenAt{};
        std::size_t framesInWindow{0};
    };

    void onConnection(const std::shared_ptr<net::TcpConnection>& connection);
    void onMessage(const std::shared_ptr<net::TcpConnection>& connection, net::Buffer* buffer);
    void processInput(const std::shared_ptr<net::TcpConnection>& connection,
                      const std::shared_ptr<ConnectionState>& state);
    void scheduleInputContinuation(const std::shared_ptr<net::TcpConnection>& connection,
                                   const std::shared_ptr<ConnectionState>& state);
    void handleFrame(const std::shared_ptr<net::TcpConnection>& connection,
                     const std::shared_ptr<ConnectionState>& state,
                     const net::framing::Packet& packet);
    AuthFrame parseAuthFrame(std::string_view payload, bool splitOnDelimiter) const;
    bool rejectAuthIfDenied(const std::shared_ptr<net::TcpConnection>& connection,
                            const std::shared_ptr<ConnectionState>& state,
                            const net::framing::Packet& packet,
                            std::string_view payload,
                            AuthFrame& authFrame);
    bool rejectIfSessionRateLimited(const std::shared_ptr<net::TcpConnection>& connection,
                                    const std::shared_ptr<ConnectionState>& state,
                                    const net::framing::Packet& packet,
                                    std::size_t payloadBytes);
    void pruneExpiredAuthReplayLocked(mini::base::Timestamp now);
    void pruneExpiredRateStatesLocked(mini::base::Timestamp now);
    static std::string authReplayKey(std::string_view sessionToken, std::string_view nonce);
    void emitMetric(GamePipelineMetricSample sample);
    void emitBackpressureMetric(GameBackpressureMetricSample sample);
    void emitSecurityMetric(GameSecurityMetricSample sample);
    void closeForSecurity(const std::shared_ptr<net::TcpConnection>& connection,
                          const std::shared_ptr<ConnectionState>& state,
                          GameSecurityMetricEvent event,
                          GameSecurityReason reason,
                          std::uint32_t msgId,
                          std::size_t payloadBytes,
                          std::size_t currentValue = 0,
                          std::size_t limit = 0,
                          std::string_view sessionTokenOverride = {});
    bool rejectInputIfOverHardLimit(const std::shared_ptr<net::TcpConnection>& connection,
                                    const std::shared_ptr<ConnectionState>& state);
    bool admitBroadcast(const net::BroadcastMetricSample& sample);

    net::TcpServer& server_;
    net::transport::TransportManager& transportManager_;
    SessionManager& sessionManager_;
    logic::LogicLoop& logicLoop_;
    Options options_;
    std::shared_ptr<void> lifetimeToken_{std::make_shared<int>(0)};

    mutable std::mutex metricMutex_;
    GamePipelineMetricCallback metricCallback_;
    GameBackpressureMetricCallback backpressureMetricCallback_;
    GameSecurityMetricCallback securityMetricCallback_;

    mutable std::mutex securityMutex_;
    AuthTokenValidator authTokenValidator_;
    std::unordered_map<std::string, mini::base::Timestamp> authReplayExpirations_;
    std::unordered_map<std::string, SessionRateState> sessionRate_;
};

}  // namespace mini::game
