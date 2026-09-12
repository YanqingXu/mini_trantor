#pragma once

// 注册表只借用 coroutine frame；共享的等待状态保护已排队恢复的有效期。
#include "mini/coroutine/CancellationToken.h"

#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>

namespace mini::net {
class EventLoop;
}

namespace mini::net::detail {

struct ConnectionAwaiterState {
    enum class Phase { Unarmed, Pending, Queued, Completed, Abandoned };
    enum class Outcome { Ready, Cancelled, Failed };
    Phase phase{Phase::Unarmed};
    Outcome outcome{Outcome::Ready};
    std::coroutine_handle<> handle{};
    std::optional<mini::coroutine::CancellationRegistration> registration;
    std::exception_ptr failure;
};

class ConnectionAwaiterRegistry {
public:
    using Waiter = std::shared_ptr<ConnectionAwaiterState>;

    explicit ConnectionAwaiterRegistry(EventLoop* loop);

    bool hasReadWaiter() const noexcept;
    void armReadWaiter(const Waiter& waiter, std::size_t minBytes, bool readyNow);
    void armWriteWaiter(const Waiter& waiter, bool readyNow);
    void armCloseWaiter(const Waiter& waiter, bool readyNow);

    void resumeReadWaiterIfSatisfied(std::size_t readableBytes);
    void resumeWriteWaiterIfNeeded();
    void resumeAllOnClose();
    bool cancelWaiter(const Waiter& waiter);
    void failWaiter(const Waiter& waiter, std::exception_ptr failure);
    // Clear the reserved slot and suppress a pending/queued completion, without resume.
    void unregisterWaiter(const Waiter& waiter);

private:
    void queueResume(const Waiter& waiter);

    EventLoop* loop_;
    Waiter readWaiter_;
    Waiter writeWaiter_;
    Waiter closeWaiter_;
    std::size_t minReadBytes_{1};
};

} // namespace mini::net::detail
