// PayloadPool — 在 owning loop 上回收并复用广播 payload 对象。

#include "mini/net/buffer/PayloadPool.h"

#include <cassert>
#include <utility>

namespace mini::net::buffer {

namespace {

void dropPayload(Payload* payload) {
    delete payload;
}

}  // namespace

PayloadPool::PayloadPool(EventLoop* ownerLoop, std::size_t maxCachedPayloads)
    : ownerLoop_(ownerLoop)
    , maxCachedPayloads_(maxCachedPayloads) {
}

PayloadPtr PayloadPool::acquire(std::string_view data) {
    Payload* rawPayload = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cachedPayloads_.empty()) {
            rawPayload = cachedPayloads_.back().release();
            cachedPayloads_.pop_back();
        }
        ++inUse_;
    }

    if (!rawPayload) {
        rawPayload = new Payload();
    }
    rawPayload->reset(data);
    return wrapWithReclaimer(rawPayload);
}

PayloadPtr PayloadPool::acquire(std::string&& data) {
    Payload* rawPayload = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cachedPayloads_.empty()) {
            rawPayload = cachedPayloads_.back().release();
            cachedPayloads_.pop_back();
        }
        ++inUse_;
    }

    if (!rawPayload) {
        rawPayload = new Payload(std::move(data));
    } else {
        rawPayload->reset(std::move(data));
    }
    return wrapWithReclaimer(rawPayload);
}

std::size_t PayloadPool::cachedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cachedPayloads_.size();
}

std::size_t PayloadPool::inUseCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return inUse_;
}

void PayloadPool::setMaxCachedPayloads(std::size_t maxCachedPayloads) {
    std::vector<std::unique_ptr<Payload>> droppedPayloads;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        maxCachedPayloads_ = maxCachedPayloads;

        while (cachedPayloads_.size() > maxCachedPayloads_) {
            droppedPayloads.push_back(std::move(cachedPayloads_.back()));
            cachedPayloads_.pop_back();
        }
    }
}

void PayloadPool::release(Payload* payload) noexcept {
    if (payload == nullptr) {
        return;
    }

    if (ownerLoop_ != nullptr && !ownerLoop_->isInLoopThread()) {
        auto self = shared_from_this();
        ownerLoop_->queueInLoop([self, payload] { self->release(payload); });
        return;
    }
    recycleInOwnerLoop(payload);
}

void PayloadPool::recycleInOwnerLoop(Payload* payload) noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        assert(inUse_ > 0);
        --inUse_;
        payload->clear();

        if (cachedPayloads_.size() < maxCachedPayloads_) {
            cachedPayloads_.push_back(std::unique_ptr<Payload>(payload));
            return;
        }
    }
    dropPayload(payload);
}

PayloadPtr PayloadPool::wrapWithReclaimer(Payload* payload) noexcept {
    auto self = weak_from_this();
    return PayloadPtr(payload, [self](Payload* released) mutable {
        if (auto pool = self.lock()) {
            pool->release(released);
            return;
        }
        dropPayload(released);
    });
}

}  // namespace mini::net::buffer
