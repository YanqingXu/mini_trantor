#pragma once

// DnsResolverOptions 收敛 DnsResolver 的配置参数。
// 值语义、可复制、默认值与 v5-gamma 行为完全一致。
// 必须在构造 DnsResolver 时设置。

#include <chrono>
#include <stdexcept>

namespace mini::net {

struct DnsResolverOptions {
    /// 工作线程数。默认 2。
    size_t numWorkerThreads = 2;

    /// 缓存 TTL。默认 60s。仅当 enableCache = true 时生效。
    std::chrono::seconds cacheTtl = std::chrono::seconds(60);

    /// 是否启用缓存。默认 false。
    bool enableCache = false;

    /// 验证选项合法性。不合法时抛 std::invalid_argument。
    void validate() const {
        if (numWorkerThreads == 0) {
            throw std::invalid_argument("DnsResolverOptions: numWorkerThreads must be > 0");
        }
        if (enableCache && cacheTtl <= std::chrono::seconds::zero()) {
            throw std::invalid_argument("DnsResolverOptions: cacheTtl must be positive when cache is enabled");
        }
    }
};

}  // namespace mini::net
