#pragma once

// GameGatewaySecurityPolicy 定义游戏网关入口的轻量安全策略。
// 它只描述认证握手、防重放和 per-session 限频配置；实际 enforcement
// 必须发生在连接 owner loop，并通过 GameServerPipeline 收敛到正常关闭路径。

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace mini::game {

struct GameSecurityOptions {
    using Duration = std::chrono::steady_clock::duration;

    std::size_t maxAuthTokenBytes{0};
    Duration authReplayWindow{Duration::zero()};
    std::string authTokenNonceDelimiter{"|"};
    std::size_t maxFramesPerSessionPerWindow{0};
    Duration sessionRateWindow{std::chrono::seconds(1)};

    void validate() const {
        if (authReplayWindow < Duration::zero()) {
            throw std::invalid_argument(
                "GameSecurityOptions::authReplayWindow must be non-negative");
        }
        if (sessionRateWindow < Duration::zero()) {
            throw std::invalid_argument(
                "GameSecurityOptions::sessionRateWindow must be non-negative");
        }
        if (maxFramesPerSessionPerWindow > 0 && sessionRateWindow <= Duration::zero()) {
            throw std::invalid_argument(
                "GameSecurityOptions::sessionRateWindow must be positive when rate limit is enabled");
        }
    }

    bool replayProtectionEnabled() const noexcept {
        return authReplayWindow > Duration::zero();
    }

    bool sessionRateLimitEnabled() const noexcept {
        return maxFramesPerSessionPerWindow > 0;
    }

    bool enabled() const noexcept {
        return maxAuthTokenBytes > 0 ||
               replayProtectionEnabled() ||
               sessionRateLimitEnabled();
    }
};

}  // namespace mini::game
