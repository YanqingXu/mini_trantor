#include "mini/net/detail/ConnectionAwaiterRegistry.h"
#include "mini/net/EventLoop.h"

#include <stdexcept>
#include <utility>

namespace mini::net::detail {

using Phase = ConnectionAwaiterState::Phase;
using Outcome = ConnectionAwaiterState::Outcome;

ConnectionAwaiterRegistry::ConnectionAwaiterRegistry(EventLoop* loop) : loop_(loop) {}

bool ConnectionAwaiterRegistry::hasReadWaiter() const noexcept {
    return static_cast<bool>(readWaiter_);
}

void ConnectionAwaiterRegistry::armReadWaiter(
    const Waiter& waiter, std::size_t minBytes, bool readyNow) {
    loop_->assertInLoopThread();
    if (readWaiter_) {
        throw std::logic_error("only one read waiter is allowed per TcpConnection");
    }
    readWaiter_ = waiter;
    minReadBytes_ = minBytes;
    if (readyNow) { queueResume(waiter); }
}

void ConnectionAwaiterRegistry::armWriteWaiter(const Waiter& waiter, bool readyNow) {
    loop_->assertInLoopThread();
    if (writeWaiter_) {
        throw std::logic_error("only one write waiter is allowed per TcpConnection");
    }
    writeWaiter_ = waiter;
    if (readyNow) { queueResume(waiter); }
}

void ConnectionAwaiterRegistry::armCloseWaiter(const Waiter& waiter, bool readyNow) {
    loop_->assertInLoopThread();
    if (closeWaiter_) {
        throw std::logic_error("only one close waiter is allowed per TcpConnection");
    }
    closeWaiter_ = waiter;
    if (readyNow) { queueResume(waiter); }
}

void ConnectionAwaiterRegistry::resumeReadWaiterIfSatisfied(std::size_t readableBytes) {
    loop_->assertInLoopThread();
    if (readableBytes >= minReadBytes_) { queueResume(readWaiter_); }
}

void ConnectionAwaiterRegistry::resumeWriteWaiterIfNeeded() {
    loop_->assertInLoopThread();
    queueResume(writeWaiter_);
}

void ConnectionAwaiterRegistry::resumeAllOnClose() {
    loop_->assertInLoopThread();
    queueResume(readWaiter_);
    queueResume(writeWaiter_);
    queueResume(closeWaiter_);
}

bool ConnectionAwaiterRegistry::cancelWaiter(const Waiter& waiter) {
    loop_->assertInLoopThread();
    if (waiter->phase != Phase::Pending) { return false; }
    waiter->outcome = Outcome::Cancelled;
    queueResume(waiter);
    return true;
}

void ConnectionAwaiterRegistry::failWaiter(const Waiter& waiter, std::exception_ptr failure) {
    loop_->assertInLoopThread();
    waiter->failure = std::move(failure);
    waiter->outcome = Outcome::Failed;
    queueResume(waiter);
}

void ConnectionAwaiterRegistry::unregisterWaiter(const Waiter& waiter) {
    loop_->assertInLoopThread();
    if (readWaiter_ == waiter) { readWaiter_.reset(); }
    if (writeWaiter_ == waiter) { writeWaiter_.reset(); }
    if (closeWaiter_ == waiter) { closeWaiter_.reset(); }
    if (waiter->phase == Phase::Pending || waiter->phase == Phase::Queued) {
        waiter->phase = Phase::Abandoned;
        waiter->handle = {};
    }
    waiter->registration.reset();
}

void ConnectionAwaiterRegistry::queueResume(const Waiter& waiter) {
    if (!waiter || waiter->phase != Phase::Pending) { return; }
    waiter->phase = Phase::Queued;
    loop_->queueInLoop([waiter] {
        if (waiter->phase != Phase::Queued) { return; }
        waiter->phase = Phase::Completed;
        auto handle = std::exchange(waiter->handle, {});
        waiter->registration.reset();
        handle.resume();
    });
}

} // namespace mini::net::detail
