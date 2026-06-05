#include "mini/net/DnsResolver.h"

#include "mini/base/Logger.h"
#include "mini/net/EventLoop.h"

#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace mini::net {

struct DnsResolver::ResolveOperationState {
    EventLoop* callbackLoop{nullptr};
    ResolveCallback callback;
    std::atomic<bool> completed{false};
    std::optional<mini::coroutine::CancellationRegistration> registration;

    void deliver(ResolveResult result) {
        if (completed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        registration.reset();
        callback(std::move(result));
    }
};

DnsResolver::DnsResolver(size_t numThreads)
    : DnsResolver(DnsResolverOptions{.numWorkerThreads = numThreads}) {
}

DnsResolver::DnsResolver(DnsResolverOptions options)
    : cacheEnabled_(options.enableCache),
      cacheTtl_(options.cacheTtl) {
    options.validate();
    for (size_t i = 0; i < options.numWorkerThreads; ++i) {
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
                          EventLoop* callbackLoop, ResolveCallback cb,
                          mini::coroutine::CancellationToken token) {
    auto operation = std::make_shared<ResolveOperationState>();
    operation->callbackLoop = callbackLoop;
    operation->callback = std::move(cb);

    if (token) {
        operation->registration.emplace(token.registerCallback([operation] {
            operation->callbackLoop->queueInLoop([operation] {
                operation->deliver(std::unexpected(NetError::Cancelled));
            });
        }));
        if (operation->completed.load(std::memory_order_acquire)) {
            return;
        }
    }

    // Check cache first.
    if (cacheEnabled_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = cache_.find(hostname);
        if (it != cache_.end() &&
            it->second.expiry > std::chrono::steady_clock::now()) {
            // Cache hit — apply requested port and deliver immediately.
            std::vector<InetAddress> addresses;
            addresses.reserve(it->second.addresses.size());
            for (const auto& sa : it->second.addresses) {
                sockaddr_storage storage = sa;
                if (storage.ss_family == AF_INET) {
                    auto* addr4 = reinterpret_cast<sockaddr_in*>(&storage);
                    addr4->sin_port = htons(port);
                } else if (storage.ss_family == AF_INET6) {
                    auto* addr6 = reinterpret_cast<sockaddr_in6*>(&storage);
                    addr6->sin6_port = htons(port);
                }
                addresses.emplace_back(storage);
            }
            callbackLoop->runInLoop(
                [operation, result = ResolveResult(std::move(addresses))]() mutable {
                    operation->deliver(std::move(result));
                });
            return;
        }
    }

    // Queue for async resolution on worker thread.
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        requestQueue_.push({hostname, port, std::move(operation)});
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

        if (req.operation->completed.load(std::memory_order_acquire)) {
            continue;
        }

        // Perform blocking resolution with AF_UNSPEC for dual-stack.
        struct addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo* res = nullptr;
        const std::string portStr = std::to_string(req.port);
        const int ret = ::getaddrinfo(req.hostname.c_str(), portStr.c_str(),
                                      &hints, &res);

        ResolveResult result = std::unexpected(NetError::ResolveFailed);
        std::vector<sockaddr_storage> cacheAddrs;
        std::vector<InetAddress> addresses;

        if (ret == 0 && res != nullptr) {
            for (const addrinfo* p = res; p != nullptr; p = p->ai_next) {
                if (p->ai_family == AF_INET && p->ai_addrlen >= sizeof(sockaddr_in)) {
                    sockaddr_storage storage{};
                    std::memcpy(&storage, p->ai_addr, sizeof(sockaddr_in));
                    addresses.emplace_back(storage);

                    // Store with port 0 for cache.
                    sockaddr_storage cacheStorage = storage;
                    reinterpret_cast<sockaddr_in*>(&cacheStorage)->sin_port = 0;
                    cacheAddrs.push_back(cacheStorage);
                } else if (p->ai_family == AF_INET6 && p->ai_addrlen >= sizeof(sockaddr_in6)) {
                    sockaddr_storage storage{};
                    std::memcpy(&storage, p->ai_addr, sizeof(sockaddr_in6));
                    addresses.emplace_back(storage);

                    // Store with port 0 for cache.
                    sockaddr_storage cacheStorage = storage;
                    reinterpret_cast<sockaddr_in6*>(&cacheStorage)->sin6_port = 0;
                    cacheAddrs.push_back(cacheStorage);
                }
            }
            ::freeaddrinfo(res);
            if (!addresses.empty()) {
                result = std::move(addresses);
            }
        } else {
            LOG_ERROR << "DnsResolver: getaddrinfo failed for '" << req.hostname << "': " << ::gai_strerror(ret);
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
        req.operation->callbackLoop->runInLoop(
            [operation = req.operation, result = std::move(result)]() mutable {
                operation->deliver(std::move(result));
            });
    }
}

}  // namespace mini::net
