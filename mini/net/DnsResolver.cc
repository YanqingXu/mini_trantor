#include "mini/net/DnsResolver.h"

#include "mini/net/EventLoop.h"

#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace mini::net {

DnsResolver::DnsResolver(size_t numThreads) {
    for (size_t i = 0; i < numThreads; ++i) {
        workers_.emplace_back([this] { workerThread(); });
    }
}

DnsResolver::~DnsResolver() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopping_ = true;
    }
    queueCv_.notify_all();
    for (auto& t : workers_) {
        t.join();
    }
}

void DnsResolver::resolve(const std::string& hostname, uint16_t port,
                           EventLoop* callbackLoop, ResolveCallback cb) {
    // Check cache first.
    if (cacheEnabled_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = cache_.find(hostname);
        if (it != cache_.end() &&
            it->second.expiry > std::chrono::steady_clock::now()) {
            // Cache hit — apply requested port and deliver immediately.
            std::vector<InetAddress> result;
            result.reserve(it->second.addresses.size());
            for (const auto& sa : it->second.addresses) {
                sockaddr_in addr = sa;
                addr.sin_port = htons(port);
                result.emplace_back(addr);
            }
            callbackLoop->runInLoop(
                [cb = std::move(cb), result = std::move(result)]() mutable {
                    cb(result);
                });
            return;
        }
    }

    // Queue for async resolution on worker thread.
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        requestQueue_.push({hostname, port, callbackLoop, std::move(cb)});
    }
    queueCv_.notify_one();
}

void DnsResolver::enableCache(std::chrono::seconds ttl) {
    cacheTtl_ = ttl;
    cacheEnabled_.store(true, std::memory_order_release);
}

void DnsResolver::clearCache() {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    cache_.clear();
}

bool DnsResolver::cacheEnabled() const noexcept {
    return cacheEnabled_.load(std::memory_order_acquire);
}

std::shared_ptr<DnsResolver> DnsResolver::getShared() {
    static auto instance = std::make_shared<DnsResolver>(2);
    return instance;
}

void DnsResolver::workerThread() {
    while (true) {
        ResolveRequest req;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] { return stopping_ || !requestQueue_.empty(); });
            if (stopping_ && requestQueue_.empty()) {
                return;
            }
            req = std::move(requestQueue_.front());
            requestQueue_.pop();
        }

        // Perform blocking resolution.
        struct addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo* res = nullptr;
        const std::string portStr = std::to_string(req.port);
        const int ret = ::getaddrinfo(req.hostname.c_str(), portStr.c_str(),
                                      &hints, &res);

        std::vector<InetAddress> addresses;
        std::vector<sockaddr_in> cacheAddrs;

        if (ret == 0 && res != nullptr) {
            for (const addrinfo* p = res; p != nullptr; p = p->ai_next) {
                if (p->ai_family == AF_INET && p->ai_addrlen >= sizeof(sockaddr_in)) {
                    sockaddr_in addr{};
                    std::memcpy(&addr, p->ai_addr, sizeof(sockaddr_in));
                    addresses.emplace_back(addr);

                    // Store with port 0 for cache.
                    sockaddr_in cacheAddr = addr;
                    cacheAddr.sin_port = 0;
                    cacheAddrs.push_back(cacheAddr);
                }
            }
            ::freeaddrinfo(res);
        } else {
            std::fprintf(stderr, "DnsResolver: getaddrinfo failed for '%s': %s\n",
                         req.hostname.c_str(), ::gai_strerror(ret));
        }

        // Update cache.
        if (cacheEnabled_.load(std::memory_order_acquire) && !cacheAddrs.empty()) {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            cache_[req.hostname] = CacheEntry{
                std::move(cacheAddrs),
                std::chrono::steady_clock::now() + cacheTtl_
            };
        }

        // Deliver result on the requesting EventLoop thread.
        req.callbackLoop->runInLoop(
            [cb = std::move(req.callback), addrs = std::move(addresses)]() mutable {
                cb(addrs);
            });
    }
}

}  // namespace mini::net
