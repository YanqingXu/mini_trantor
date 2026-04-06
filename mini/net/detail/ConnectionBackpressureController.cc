#include "mini/net/detail/ConnectionBackpressureController.h"

#include "mini/net/Channel.h"

#include <stdexcept>

namespace mini::net::detail {

ConnectionBackpressureController::ConnectionBackpressureController(EventLoop* loop) : loop_(loop) {
}

void ConnectionBackpressureController::validateThresholds(
    std::size_t highWaterMark,
    std::size_t lowWaterMark) {
    if (highWaterMark == 0) {
        if (lowWaterMark != 0) {
            throw std::invalid_argument("backpressure low water mark requires a non-zero high water mark");
        }
        return;
    }

    if (lowWaterMark >= highWaterMark) {
        throw std::invalid_argument("backpressure low water mark must be smaller than high water mark");
    }
}

void ConnectionBackpressureController::configure(
    std::size_t highWaterMark,
    std::size_t lowWaterMark,
    std::size_t bufferedBytes,
    Channel& channel) {
    loop_->assertInLoopThread();
    highWaterMark_ = highWaterMark;
    lowWaterMark_ = lowWaterMark;
    apply(bufferedBytes, channel, active_);
}

void ConnectionBackpressureController::onConnectionEstablished(
    std::size_t bufferedBytes,
    Channel& channel) {
    loop_->assertInLoopThread();
    active_ = true;
    readingEnabled_ = true;
    apply(bufferedBytes, channel, active_);
}

void ConnectionBackpressureController::onBufferedBytesChanged(
    std::size_t bufferedBytes,
    Channel& channel) {
    loop_->assertInLoopThread();
    apply(bufferedBytes, channel, active_);
}

void ConnectionBackpressureController::onClosed() {
    loop_->assertInLoopThread();
    active_ = false;
    readingEnabled_ = false;
}

bool ConnectionBackpressureController::readingEnabled() const noexcept {
    return readingEnabled_;
}

std::size_t ConnectionBackpressureController::highWaterMark() const noexcept {
    return highWaterMark_;
}

std::size_t ConnectionBackpressureController::lowWaterMark() const noexcept {
    return lowWaterMark_;
}

void ConnectionBackpressureController::apply(
    std::size_t bufferedBytes,
    Channel& channel,
    bool active) {
    loop_->assertInLoopThread();
    if (!active) {
        return;
    }

    if (highWaterMark_ == 0) {
        if (!readingEnabled_) {
            readingEnabled_ = true;
            channel.enableReading();
        }
        return;
    }

    if (readingEnabled_ && bufferedBytes >= highWaterMark_) {
        readingEnabled_ = false;
        channel.disableReading();
        return;
    }

    if (!readingEnabled_ && bufferedBytes <= lowWaterMark_) {
        readingEnabled_ = true;
        channel.enableReading();
    }
}

}  // namespace mini::net::detail
