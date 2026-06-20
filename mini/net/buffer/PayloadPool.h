// PayloadPool — 广播载荷对象池（用于共享与回收）。

#pragma once

#include "mini/net/buffer/Payload.h"
#include "mini/net/EventLoop.h"
#include "mini/base/noncopyable.h"

#include <cassert>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace mini::net::buffer {

class PayloadPool : private mini::base::noncopyable, public std::enable_shared_from_this<PayloadPool> {
public:
    static constexpr std::size_t kDefaultMaxCached = 64;

    explicit PayloadPool(EventLoop* ownerLoop, std::size_t maxCachedPayloads = kDefaultMaxCached);

    PayloadPtr acquire(std::string_view data);
    PayloadPtr acquire(std::string&& data);

    std::size_t cachedCount() const;
    std::size_t inUseCount() const;

    void release(Payload* payload) noexcept;
    void setMaxCachedPayloads(std::size_t maxCachedPayloads);

private:
    void recycleInOwnerLoop(Payload* payload) noexcept;
    PayloadPtr wrapWithReclaimer(Payload* payload) noexcept;

    EventLoop* ownerLoop_{nullptr};
    std::size_t maxCachedPayloads_{kDefaultMaxCached};
    mutable std::mutex mutex_;
    std::size_t inUse_{0};
    std::vector<std::unique_ptr<Payload>> cachedPayloads_;
};

}  // namespace mini::net::buffer
