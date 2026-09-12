#pragma once

// TcpServerOptions 收敛 TcpServer 的配置参数。
// 值语义、可复制；numThreads 表示 base loop 之外的 worker 数。
// 必须在 start() 前设置。与现有 set*() 方法并存，set*() 可覆盖 Options 值。

#include <chrono>
#include <cstddef>
#include <stdexcept>

namespace mini::net {

struct TcpServerOptions {
    using Duration = std::chrono::steady_clock::duration;

    struct MetricsOptions {
        /// Enable EventLoop pending-functor / wakeup samples for base and worker loops.
        bool enableEventLoopQueueMetrics = false;
    };

    /// worker IO 线程数（不含 base loop）。0 为单线程；默认 1 个 worker。
    int numThreads = 1;

    /// 空闲连接超时。默认 0 表示不超时。
    Duration idleTimeout = Duration::zero();

    /// 背压高水位。输出缓冲区字节数 >= 此值时暂停读取。默认 0 表示禁用。
    std::size_t backpressureHighWaterMark = 0;

    /// 背压低水位。输出缓冲区字节数 <= 此值时恢复读取。默认 0。
    /// 仅在 backpressureHighWaterMark > 0 时生效，且必须 < highWaterMark。
    std::size_t backpressureLowWaterMark = 0;

    /// 是否允许端口复用（SO_REUSEPORT）。默认 true。
    bool reusePort = true;

    /// 网络行为指标开关。默认关闭，保持历史路径的最小开销。
    MetricsOptions metrics{};

    /// 验证选项合法性。不合法时抛 std::invalid_argument。
    void validate() const {
        if (numThreads < 0) {
            throw std::invalid_argument("TcpServerOptions: numThreads must be >= 0");
        }
        if (idleTimeout < Duration::zero()) {
            throw std::invalid_argument("TcpServerOptions: idleTimeout must be non-negative");
        }
        validateBackpressure(backpressureHighWaterMark, backpressureLowWaterMark);
    }

    /// 验证背压阈值。公共静态方法，供 TcpServer::setBackpressurePolicy 复用。
    static void validateBackpressure(std::size_t highWaterMark, std::size_t lowWaterMark) {
        if (highWaterMark == 0) {
            if (lowWaterMark != 0) {
                throw std::invalid_argument(
                    "TcpServerOptions: backpressureLowWaterMark requires a non-zero backpressureHighWaterMark");
            }
        } else if (lowWaterMark >= highWaterMark) {
            throw std::invalid_argument(
                "TcpServerOptions: backpressureLowWaterMark must be smaller than backpressureHighWaterMark");
        }
    }
};

}  // namespace mini::net
