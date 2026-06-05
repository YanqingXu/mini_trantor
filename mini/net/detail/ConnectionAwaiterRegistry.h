#pragma once

#include "mini/net/EventLoop.h"

#include <coroutine>
#include <cstddef>

namespace mini::net::detail {

class ConnectionAwaiterRegistry {
public:
    explicit ConnectionAwaiterRegistry(EventLoop* loop);

    bool hasReadWaiter() const noexcept;

    void armReadWaiter(std::coroutine_handle<> handle, std::size_t minBytes, bool readyNow);
    void armWriteWaiter(std::coroutine_handle<> handle, bool readyNow);
    void armCloseWaiter(std::coroutine_handle<> handle, bool readyNow);

    void resumeReadWaiterIfSatisfied(std::size_t readableBytes);
    void resumeWriteWaiterIfNeeded();
    void resumeAllOnClose();
    bool cancelReadWaiter(std::coroutine_handle<> handle);
    bool cancelWriteWaiter(std::coroutine_handle<> handle);
    bool cancelCloseWaiter(std::coroutine_handle<> handle);

private:
    struct ReadAwaiterState {
        std::coroutine_handle<> handle{};
        std::size_t minBytes{1};
        bool active{false};
    };

    struct WaiterState {
        std::coroutine_handle<> handle{};
        bool active{false};
    };

    void queueResume(std::coroutine_handle<> handle);

    EventLoop* loop_;
    ReadAwaiterState readWaiter_;
    WaiterState writeWaiter_;
    WaiterState closeWaiter_;
};

}  // namespace mini::net::detail
