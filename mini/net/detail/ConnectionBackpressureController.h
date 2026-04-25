#pragma once

#include "mini/base/MetricsHook.h"
#include "mini/net/EventLoop.h"

#include <cstddef>
#include <functional>

namespace mini::net {
class Channel;
}

namespace mini::net::detail {

class ConnectionBackpressureController {
public:
    using BackpressureEventCallback = mini::net::BackpressureEventCallback;

    explicit ConnectionBackpressureController(EventLoop* loop);

    static void validateThresholds(std::size_t highWaterMark, std::size_t lowWaterMark);

    void configure(
        std::size_t highWaterMark,
        std::size_t lowWaterMark,
        std::size_t bufferedBytes,
        Channel& channel);
    void onConnectionEstablished(std::size_t bufferedBytes, Channel& channel);
    void onBufferedBytesChanged(std::size_t bufferedBytes, Channel& channel);
    void onClosed();

    bool readingEnabled() const noexcept;
    std::size_t highWaterMark() const noexcept;
    std::size_t lowWaterMark() const noexcept;

    /// Set backpressure event hook. Fires on owner loop thread.
    void setBackpressureEventCallback(BackpressureEventCallback cb);

    /// Set the TcpConnection shared_ptr for hook callback.
    void setConnectionPtr(std::shared_ptr<class TcpConnection> conn);

private:
    void apply(std::size_t bufferedBytes, Channel& channel, bool active);

    EventLoop* loop_;
    std::size_t highWaterMark_{0};
    std::size_t lowWaterMark_{0};
    bool active_{false};
    bool readingEnabled_{true};

    // Metrics hook (v5-delta)
    BackpressureEventCallback backpressureEventCallback_;
    std::weak_ptr<class TcpConnection> connection_;
};

}  // namespace mini::net::detail
