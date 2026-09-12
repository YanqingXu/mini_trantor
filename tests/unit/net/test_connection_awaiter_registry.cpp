#include "mini/coroutine/Task.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/detail/ConnectionAwaiterRegistry.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {

mini::coroutine::Task<void> waitForRead(
    mini::net::detail::ConnectionAwaiterRegistry* registry,
    std::promise<std::thread::id>* resumedOn,
    std::shared_ptr<mini::net::detail::ConnectionAwaiterState>* exposed = nullptr) {
    struct Awaitable {
        mini::net::detail::ConnectionAwaiterRegistry* registry;
        std::shared_ptr<mini::net::detail::ConnectionAwaiterState> state =
            std::make_shared<mini::net::detail::ConnectionAwaiterState>();

        ~Awaitable() { registry->unregisterWaiter(state); }

        bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(mini::coroutine::Task<void>::handle_type handle) {
            state->handle = mini::coroutine::detail::borrowResume(handle);
            state->phase = mini::net::detail::ConnectionAwaiterState::Phase::Pending;
            registry->armReadWaiter(state, 4, false);
        }

        void await_resume() const noexcept {
            registry->unregisterWaiter(state);
        }
    };

    Awaitable waiting{registry};
    if (exposed) { *exposed = waiting.state; }
    co_await waiting;
    resumedOn->set_value(std::this_thread::get_id());
}

}  // namespace

int main() {
    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* loop = loopThread.startLoop();
    auto registry = std::make_shared<mini::net::detail::ConnectionAwaiterRegistry>(loop);
    auto* regPtr = registry.get();

    std::promise<std::thread::id> ownerThreadPromise;
    auto ownerThreadFuture = ownerThreadPromise.get_future();
    loop->runInLoop([&ownerThreadPromise] { ownerThreadPromise.set_value(std::this_thread::get_id()); });
    assert(ownerThreadFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    const auto ownerThread = ownerThreadFuture.get();

    std::promise<std::thread::id> resumedOnPromise;
    auto resumedOnFuture = resumedOnPromise.get_future();
    auto first = waitForRead(regPtr, &resumedOnPromise);

    std::promise<void> firstStarted;
    auto firstStartedFuture = firstStarted.get_future();
    loop->runInLoop([&] {
        first.start();
        firstStarted.set_value();
    });
    assert(firstStartedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(regPtr->hasReadWaiter());

    std::promise<void> partialObserved;
    auto partialObservedFuture = partialObserved.get_future();
    loop->runInLoop([&] {
        regPtr->resumeReadWaiterIfSatisfied(2);
        partialObserved.set_value();
    });
    assert(partialObservedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(resumedOnFuture.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);

    std::promise<void> fullObserved;
    auto fullObservedFuture = fullObserved.get_future();
    loop->runInLoop([&] {
        regPtr->resumeReadWaiterIfSatisfied(4);
        fullObserved.set_value();
    });
    assert(fullObservedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(resumedOnFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(resumedOnFuture.get() == ownerThread);

    std::promise<void> firstDrained;
    auto firstDrainedFuture = firstDrained.get_future();
    loop->runInLoop([&] { firstDrained.set_value(); });
    assert(firstDrainedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    // The coroutine is resumed via queueResume (which uses queueInLoop), so
    // we need to ensure the loop has processed that callback before checking done().
    // Wait until the coroutine reports done, with a timeout.
    {
        bool done = false;
        for (int i = 0; i < 100; ++i) {
            if (first.done()) { done = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!done) {
            std::abort();
        }
    }
    first.result();

    std::promise<std::thread::id> duplicateResumePromise;
    auto duplicateResumeFuture = duplicateResumePromise.get_future();
    auto duplicateFirst = waitForRead(regPtr, &duplicateResumePromise);
    auto duplicateSecond = waitForRead(regPtr, &duplicateResumePromise);

    std::promise<void> duplicateStarted;
    auto duplicateStartedFuture = duplicateStarted.get_future();
    loop->runInLoop([&] {
        duplicateFirst.start();
        duplicateSecond.start();
        duplicateStarted.set_value();
    });
    assert(duplicateStartedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(duplicateSecond.done());

    bool rejected = false;
    try {
        duplicateSecond.result();
    } catch (const std::logic_error& error) {
        rejected = true;
        assert(std::string_view(error.what()) == "only one read waiter is allowed per TcpConnection");
    }
    assert(rejected);

    std::promise<void> closeObserved;
    auto closeObservedFuture = closeObserved.get_future();
    loop->runInLoop([&] {
        regPtr->resumeAllOnClose();
        closeObserved.set_value();
    });
    assert(closeObservedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(duplicateResumeFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(duplicateResumeFuture.get() == ownerThread);

    std::promise<void> duplicateDrained;
    auto duplicateDrainedFuture = duplicateDrained.get_future();
    loop->runInLoop([&] { duplicateDrained.set_value(); });
    assert(duplicateDrainedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    // Wait until duplicateFirst reports done, with a timeout.
    {
        bool done = false;
        for (int i = 0; i < 100; ++i) {
            if (duplicateFirst.done()) { done = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!done) {
            std::abort();
        }
    }
    duplicateFirst.result();

    // Exercise the cancellation section formerly skipped because of stale handles.
    std::shared_ptr<mini::net::detail::ConnectionAwaiterState> cancelledState;
    std::promise<std::thread::id> cancelledOn;
    auto cancelledOnFuture = cancelledOn.get_future();
    auto cancelled = waitForRead(regPtr, &cancelledOn, &cancelledState);
    loop->queueInLoop([&] {
        cancelled.start();
        const bool accepted = regPtr->cancelWaiter(cancelledState);
        assert(accepted);
        const bool again = regPtr->cancelWaiter(cancelledState);
        assert(!again);
        regPtr->resumeAllOnClose(); // already queued: must not resume twice
    });
    assert(cancelledOnFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(cancelledOnFuture.get() == ownerThread);

    std::promise<void> cleaned;
    auto cleanedFuture = cleaned.get_future();
    loop->queueInLoop([&] {
        assert(cancelled.done());
        cancelled.result();
        cancelled = {};
        assert(!regPtr->hasReadWaiter());
        cleaned.set_value();
    });
    assert(cleanedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);

    std::promise<std::thread::id> abandonedResume;
    auto abandonedResumeFuture = abandonedResume.get_future();
    std::promise<void> abandonedDrained;
    auto abandonedDrainedFuture = abandonedDrained.get_future();
    loop->queueInLoop([&] {
        std::shared_ptr<mini::net::detail::ConnectionAwaiterState> state;
        auto abandoned = waitForRead(regPtr, &abandonedResume, &state);
        abandoned.start();
        const bool accepted = regPtr->cancelWaiter(state);
        assert(accepted);
        abandoned = {}; // invalidates the callback that was just queued
        assert(!regPtr->hasReadWaiter());
        loop->queueInLoop([&] { abandonedDrained.set_value(); });
    });
    assert(abandonedDrainedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(abandonedResumeFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

    return 0;
}
