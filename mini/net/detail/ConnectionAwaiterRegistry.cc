#include "mini/net/detail/ConnectionAwaiterRegistry.h"

#include <stdexcept>

namespace mini::net::detail {

ConnectionAwaiterRegistry::ConnectionAwaiterRegistry(EventLoop* loop) : loop_(loop) {
}

bool ConnectionAwaiterRegistry::hasReadWaiter() const noexcept {
    return readWaiter_.active;
}

void ConnectionAwaiterRegistry::armReadWaiter(
    std::coroutine_handle<> handle,
    std::size_t minBytes,
    bool readyNow) {
    loop_->assertInLoopThread();
    if (readWaiter_.active) {
        throw std::logic_error("only one read waiter is allowed per TcpConnection");
    }
    if (readyNow) {
        queueResume(handle);
        return;
    }
    readWaiter_ = {.handle = handle, .minBytes = minBytes, .active = true};
}

void ConnectionAwaiterRegistry::armWriteWaiter(std::coroutine_handle<> handle, bool readyNow) {
    loop_->assertInLoopThread();
    if (writeWaiter_.active) {
        throw std::logic_error("only one write waiter is allowed per TcpConnection");
    }
    if (readyNow) {
        queueResume(handle);
        return;
    }
    writeWaiter_ = {.handle = handle, .active = true};
}

void ConnectionAwaiterRegistry::armCloseWaiter(std::coroutine_handle<> handle, bool readyNow) {
    loop_->assertInLoopThread();
    if (closeWaiter_.active) {
        throw std::logic_error("only one close waiter is allowed per TcpConnection");
    }
    if (readyNow) {
        queueResume(handle);
        return;
    }
    closeWaiter_ = {.handle = handle, .active = true};
}

void ConnectionAwaiterRegistry::resumeReadWaiterIfSatisfied(std::size_t readableBytes) {
    loop_->assertInLoopThread();
    if (!readWaiter_.active) {
        return;
    }
    if (readableBytes < readWaiter_.minBytes) {
        return;
    }
    auto handle = readWaiter_.handle;
    readWaiter_ = {};
    queueResume(handle);
}

void ConnectionAwaiterRegistry::resumeWriteWaiterIfNeeded() {
    loop_->assertInLoopThread();
    if (!writeWaiter_.active) {
        return;
    }
    auto handle = writeWaiter_.handle;
    writeWaiter_ = {};
    queueResume(handle);
}

void ConnectionAwaiterRegistry::resumeAllOnClose() {
    loop_->assertInLoopThread();
    if (readWaiter_.active) {
        auto handle = readWaiter_.handle;
        readWaiter_ = {};
        queueResume(handle);
    }
    if (writeWaiter_.active) {
        auto handle = writeWaiter_.handle;
        writeWaiter_ = {};
        queueResume(handle);
    }
    if (closeWaiter_.active) {
        auto handle = closeWaiter_.handle;
        closeWaiter_ = {};
        queueResume(handle);
    }
}

bool ConnectionAwaiterRegistry::cancelReadWaiter(std::coroutine_handle<> handle) {
    loop_->assertInLoopThread();
    if (!readWaiter_.active || readWaiter_.handle != handle) {
        return false;
    }
    auto waiter = readWaiter_.handle;
    readWaiter_ = {};
    queueResume(waiter);
    return true;
}

bool ConnectionAwaiterRegistry::cancelWriteWaiter(std::coroutine_handle<> handle) {
    loop_->assertInLoopThread();
    if (!writeWaiter_.active || writeWaiter_.handle != handle) {
        return false;
    }
    auto waiter = writeWaiter_.handle;
    writeWaiter_ = {};
    queueResume(waiter);
    return true;
}

bool ConnectionAwaiterRegistry::cancelCloseWaiter(std::coroutine_handle<> handle) {
    loop_->assertInLoopThread();
    if (!closeWaiter_.active || closeWaiter_.handle != handle) {
        return false;
    }
    auto waiter = closeWaiter_.handle;
    closeWaiter_ = {};
    queueResume(waiter);
    return true;
}

void ConnectionAwaiterRegistry::queueResume(std::coroutine_handle<> handle) {
    if (!handle) {
        return;
    }
    loop_->queueInLoop([handle] { handle.resume(); });
}

}  // namespace mini::net::detail
