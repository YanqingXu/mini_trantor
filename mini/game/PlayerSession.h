#pragma once

// PlayerSession — 游戏会话抽象。
//
// 设计目标：
// - 只表达会话级生命周期（身份、认证、心跳、重连入口）
// - 不直接依赖具体传输类型，仅持有 transportSessionId 进行引用
// - 状态变更保持幂等，便于重复事件（重入）下安全处理
//
// 线程语义：
// - 任何线程可查询只读信息；状态迁移建议集中在逻辑线程统一入口调用。

#include "mini/base/Timestamp.h"
#include "mini/net/transport/TransportTypes.h"

#include <any>
#include <chrono>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>

namespace mini::game {

class PlayerSession {
public:
    using SessionToken = std::string;
    using TimePoint = std::chrono::steady_clock::time_point;
    using Milliseconds = std::chrono::milliseconds;

    enum class State {
        kCreated,
        kAuthenticating,
        kAuthenticated,
        kOnline,
        kHeartbeatTimeout,
        kReconnecting,
        kClosing,
        kClosed
    };

    static constexpr Milliseconds kDefaultAuthTimeout{15000};
    static constexpr Milliseconds kDefaultHeartbeatTimeout{30000};

    PlayerSession(SessionToken sessionToken,
                 mini::net::transport::TransportSessionId transportSessionId =
                     mini::net::transport::kInvalidTransportSessionId,
                 Milliseconds authTimeout = kDefaultAuthTimeout,
                 Milliseconds heartbeatTimeout = kDefaultHeartbeatTimeout);

    const SessionToken& sessionId() const noexcept;
    mini::net::transport::TransportSessionId transportSessionId() const noexcept;

    State state() const;
    static const char* stateName(State state);
    const char* stateName() const;

    bool startAuthentication(TimePoint now = mini::base::now());
    bool markAuthenticated(std::uint64_t playerId,
                          std::string_view playerName,
                          std::string_view role = {});
    bool markOnline(TimePoint now = mini::base::now());
    bool refreshHeartbeat(TimePoint now = mini::base::now());
    bool onAuthTimeout(TimePoint now = mini::base::now());
    bool onHeartbeatTimeout(TimePoint now = mini::base::now());
    bool onConnectionClose(std::string_view reason);
    bool markReconnecting();
    bool close(std::string_view reason);

    bool bindTransportSession(mini::net::transport::TransportSessionId id);
    bool detachTransport();
    bool hasTransport() const;

    std::uint64_t playerId() const;
    std::string playerName() const;
    std::string role() const;
    std::string closeReason() const;

    TimePoint createdAt() const;
    TimePoint lastActivityAt() const;
    TimePoint lastHeartbeatAt() const;

    void setUserContext(std::any ctx);
    std::any userContext() const;
    bool hasUserContext() const;

    bool isOnline() const;
    bool isClosed() const;
    bool isClosingOrClosed() const;
    bool isAuthenticating() const;
    bool isReconnecting() const;

private:
    void transition(State next);

    SessionToken sessionToken_;
    mini::net::transport::TransportSessionId transportSessionId_;
    State state_{State::kCreated};
    TimePoint createdAt_{};
    TimePoint lastActivityAt_{};
    TimePoint lastHeartbeatAt_{};
    TimePoint authDeadline_{};
    Milliseconds authTimeout_;
    Milliseconds heartbeatTimeout_;
    std::uint64_t playerId_{0};
    std::string playerName_;
    std::string role_;
    std::string closeReason_;
    std::any userContext_;

    mutable std::mutex mutex_;
};

using PlayerSessionPtr = std::shared_ptr<PlayerSession>;

}  // namespace mini::game
