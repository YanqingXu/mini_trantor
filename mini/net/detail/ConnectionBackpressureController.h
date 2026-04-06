#pragma once

#include "mini/net/EventLoop.h"

#include <cstddef>

namespace mini::net {
class Channel;
}

namespace mini::net::detail {

class ConnectionBackpressureController {
public:
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

private:
    void apply(std::size_t bufferedBytes, Channel& channel, bool active);

    EventLoop* loop_;
    std::size_t highWaterMark_{0};
    std::size_t lowWaterMark_{0};
    bool active_{false};
    bool readingEnabled_{true};
};

}  // namespace mini::net::detail
