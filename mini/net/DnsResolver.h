#pragma once

// DnsResolver 提供异步域名解析，使用工作线程池执行阻塞 getaddrinfo，
// 通过 EventLoop::runInLoop 将结果投递回请求方线程。
// 支持可选的 TTL 缓存。不阻塞任何 EventLoop 线程。

#include "mini/base/noncopyable.h"
#include "mini/net/InetAddress.h"
#include "mini/net/NetError.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mini::net {

class EventLoop;

class DnsResolver : private mini::base::noncopyable {
public:
    using ResolveResult = Expected<std::vector<InetAddress>>;
    using ResolveCallback = std::function<void(ResolveResult)>;

    /// Create a resolver with the given number of worker threads.
    explicit DnsResolver(size_t numThreads = 2);
    ~DnsResolver();

    /// Resolve hostname asynchronously.
    /// Callback is delivered on callbackLoop's thread with either resolved
    /// addresses or an explicit ResolveFailed error.
    void resolve(const std::string& hostname, uint16_t port,
                 EventLoop* callbackLoop, ResolveCallback cb);

    /// Enable caching with the given TTL. Default TTL: 60 seconds.
    void enableCache(std::chrono::seconds ttl = std::chrono::seconds(60));

    /// Clear all cached entries.
    void clearCache();

    /// Whether cache is enabled.
    bool cacheEnabled() const noexcept;

    /// Get the global shared resolver instance (2 worker threads).
    static std::shared_ptr<DnsResolver> getShared();

private:
    void workerThread();

    struct ResolveRequest {
        std::string hostname;
        uint16_t port;
        EventLoop* callbackLoop;
        ResolveCallback callback;
    };

    struct CacheEntry {
        std::vector<sockaddr_in> addresses;  // stored with port 0
        std::chrono::steady_clock::time_point expiry;
    };

    // Worker thread pool
    std::vector<std::thread> workers_;
    std::queue<ResolveRequest> requestQueue_;
    mutable std::mutex queueMutex_;
    std::condition_variable queueCv_;
    bool stopping_{false};

    // Cache
    std::atomic<bool> cacheEnabled_{false};
    std::chrono::seconds cacheTtl_{60};
    mutable std::mutex cacheMutex_;
    std::unordered_map<std::string, CacheEntry> cache_;
};

}  // namespace mini::net
