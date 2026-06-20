#pragma once

// SessionManager — 会话层调度器。
//
// 1) 持有 shared_ptr<PlayerSession> 作为 session-owner。
// 2) 网络侧只提交 sessionToken/transportId，状态推进在 manager 统一入口。
// 3) 状态变更回调默认投递到逻辑 loop，避免跨线程回调重入。

#include "mini/base/MetricsHook.h"
#include "mini/game/PlayerSession.h"
#include "mini/net/EventLoop.h"
#include "mini/net/transport/ITransport.h"

#include <chrono>
#include <memory>
#include <functional>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mini::game {

class SessionManager {
public:
    static constexpr PlayerSession::Milliseconds kDefaultReconnectWindow{15000};

    using SessionToken = PlayerSession::SessionToken;
    using State = PlayerSession::State;
    using SessionStateCallback =
        std::function<void(const SessionToken& sessionToken,
                           State oldState,
                           State newState,
                           std::string_view reason)>;

    explicit SessionManager(mini::net::EventLoop* logicLoop = nullptr,
                           PlayerSession::Milliseconds reconnectWindow = kDefaultReconnectWindow)
        : logicLoop_(logicLoop) {
        setReconnectWindow(reconnectWindow);
    }

    void setOwnerLoop(mini::net::EventLoop* loop) {
        logicLoop_ = loop;
    }

    mini::net::EventLoop* ownerLoop() const noexcept {
        return logicLoop_;
    }

    void setStateCallback(SessionStateCallback callback) {
        std::scoped_lock lock(mutex_);
        stateCallback_ = std::move(callback);
    }

    void setMetricCallback(SessionMetricCallback callback) {
        std::scoped_lock lock(mutex_);
        metricCallback_ = std::move(callback);
    }

    PlayerSessionPtr ensureSession(
        SessionToken sessionToken,
        mini::net::transport::TransportSessionId transportSessionId,
        bool autoStartAuth = true);

    PlayerSessionPtr getSession(std::string_view sessionToken) const;
    std::weak_ptr<PlayerSession> getSessionWeak(std::string_view sessionToken) const;
    PlayerSessionPtr getSession(mini::net::transport::TransportSessionId transportSessionId) const;

    bool hasSession(std::string_view sessionToken) const;
    std::size_t sessionCount() const;

    bool markStartAuth(std::string_view sessionToken);
    bool authenticate(std::string_view sessionToken,
                     std::uint64_t playerId,
                     std::string_view playerName,
                     std::string_view role = {});
    bool markOnline(std::string_view sessionToken);
    bool refreshHeartbeat(std::string_view sessionToken);
    bool onAuthTimeout(std::string_view sessionToken);
    bool onHeartbeatTimeout(std::string_view sessionToken);
    bool onConnectionClose(mini::net::transport::TransportSessionId transportSessionId,
                          std::string_view reason = "connection closed");
    bool markReconnecting(std::string_view sessionToken);
    bool markClosed(std::string_view sessionToken, std::string_view reason = "closed by manager");
    bool bindTransport(std::string_view sessionToken,
                      mini::net::transport::TransportSessionId transportSessionId);
    bool bindTransportEndpoint(
        std::string_view sessionToken,
        const std::shared_ptr<mini::net::transport::ITransportEndpoint>& endpoint);
    bool onReconnect(std::string_view sessionToken,
                     mini::net::transport::TransportSessionId transportSessionId);

    bool removeSession(std::string_view sessionToken);
    bool removeSessionByTransport(mini::net::transport::TransportSessionId transportSessionId);
    bool cleanupClosedSessions(
        std::chrono::steady_clock::time_point now = mini::base::now());

    void setReconnectWindow(PlayerSession::Milliseconds reconnectWindow);
    PlayerSession::Milliseconds reconnectWindow() const;
    std::shared_ptr<mini::net::transport::ITransportEndpoint>
    getTransportEndpoint(std::string_view sessionToken) const;
    std::shared_ptr<mini::net::transport::ITransportEndpoint>
    getTransportEndpoint(mini::net::transport::TransportSessionId transportSessionId) const;

private:
    template <typename Fn>
    void postOnLogicLoop(Fn&& fn) const;

    void emitState(const SessionToken& sessionToken,
                   State oldState,
                   State newState,
                   std::string_view reason);
    void emitSessionMetric(SessionMetricSample sample);

    bool setTransportIndexLocked(
        const SessionToken& sessionToken,
        mini::net::transport::TransportSessionId oldTransportId,
        mini::net::transport::TransportSessionId newTransportId,
        const PlayerSessionPtr& session);

    void scheduleReconnectWindow(const SessionToken& sessionToken);
    void cancelReconnectWindow(const SessionToken& sessionToken);
    void cancelReconnectWindowLocked(const SessionToken& sessionToken);
    void onReconnectWindowExpired(const SessionToken& sessionToken, std::uint64_t epoch);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, PlayerSessionPtr> sessions_;
    std::unordered_map<mini::net::transport::TransportSessionId, std::string> transportIndex_;
    std::unordered_map<
        mini::net::transport::TransportSessionId,
        std::weak_ptr<mini::net::transport::ITransportEndpoint>>
        endpointIndex_;
    std::unordered_map<SessionToken, mini::net::TimerId> reconnectTimer_;
    std::unordered_map<SessionToken, std::uint64_t> reconnectEpoch_;
    std::unordered_map<SessionToken, PlayerSession::TimePoint> reconnectStartedAt_;

    SessionStateCallback stateCallback_;
    SessionMetricCallback metricCallback_;
    mini::net::EventLoop* logicLoop_{nullptr};
    PlayerSession::Milliseconds reconnectWindow_{kDefaultReconnectWindow};
};

}  // namespace mini::game
