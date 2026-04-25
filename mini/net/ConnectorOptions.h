#pragma once

// ConnectorOptions 收敛 Connector 的配置参数。
// 值语义、可复制、默认值与 v5-gamma 行为完全一致。
// 必须在 start() 前设置。

#include <chrono>
#include <stdexcept>

namespace mini::net {

struct ConnectorOptions {
    using Duration = std::chrono::steady_clock::duration;

    /// 初始重试延迟。默认 500ms。
    Duration initRetryDelay = std::chrono::milliseconds(500);

    /// 最大重试延迟。默认 30s。指数退避不超过此值。
    Duration maxRetryDelay = std::chrono::seconds(30);

    /// 连接超时。默认 0 表示不超时（依赖 TCP 栈默认约 2min）。
    /// 设置 > 0 时，连接超时后关闭 socket 并触发重试或失败。
    Duration connectTimeout = Duration::zero();

    /// 是否在连接失败后自动重试。默认 false。
    /// 注意：TcpClient 的 enableRetry() 也会控制此行为。
    bool enableRetry = false;

    /// 验证选项合法性。不合法时抛 std::invalid_argument。
    void validate() const {
        if (initRetryDelay <= Duration::zero()) {
            throw std::invalid_argument("ConnectorOptions: initRetryDelay must be positive");
        }
        if (maxRetryDelay < initRetryDelay) {
            throw std::invalid_argument("ConnectorOptions: maxRetryDelay must be >= initRetryDelay");
        }
        if (connectTimeout < Duration::zero()) {
            throw std::invalid_argument("ConnectorOptions: connectTimeout must be non-negative");
        }
    }
};

}  // namespace mini::net
