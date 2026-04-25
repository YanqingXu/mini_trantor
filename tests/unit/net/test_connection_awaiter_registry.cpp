#include "mini/coroutine/Task.h"
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
    std::promise<std::thread::id>* resumedOn) {
    struct Awaitable {
        mini::net::detail::ConnectionAwaiterRegistry* registry;

        bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle) {
            registry->armReadWaiter(handle, 4, false);
        }

        void await_resume() const noexcept {
        }
    };

    co_await Awaitable{registry};
    resumedOn->set_value(std::this_thread::get_id());
}

}  // namespace

int main() {
    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* loop = loopThread.startLoop();
    mini::net::detail::ConnectionAwaiterRegistry registry(loop);

    std::promise<std::thread::id> ownerThreadPromise;
    auto ownerThreadFuture = ownerThreadPromise.get_future();
    loop->runInLoop([&ownerThreadPromise] { ownerThreadPromise.set_value(std::this_thread::get_id()); });
    assert(ownerThreadFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    const auto ownerThread = ownerThreadFuture.get();

    std::promise<std::thread::id> resumedOnPromise;
    auto resumedOnFuture = resumedOnPromise.get_future();
    auto first = waitForRead(&registry, &resumedOnPromise);

    std::promise<void> firstStarted;
    auto firstStartedFuture = firstStarted.get_future();
    loop->runInLoop([&] {
        first.start();
        firstStarted.set_value();
    });
    assert(firstStartedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(registry.hasReadWaiter());

    std::promise<void> partialObserved;
    auto partialObservedFuture = partialObserved.get_future();
    loop->runInLoop([&] {
        registry.resumeReadWaiterIfSatisfied(2);
        partialObserved.set_value();
    });
    assert(partialObservedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(resumedOnFuture.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);

    std::promise<void> fullObserved;
    auto fullObservedFuture = fullObserved.get_future();
    loop->runInLoop([&] {
        registry.resumeReadWaiterIfSatisfied(4);
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
    auto duplicateFirst = waitForRead(&registry, &duplicateResumePromise);
    auto duplicateSecond = waitForRead(&registry, &duplicateResumePromise);

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
        registry.resumeAllOnClose();
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

    std::coroutine_handle<> storedHandle{};
    auto cancelledResumePromise = std::make_shared<std::promise<std::thread::id>>();
    auto cancelledResumeFuture = cancelledResumePromise->get_future();

    auto sharedState = std::make_shared<std::coroutine_handle<>>();
    // Wrap the registry pointer in a shared_ptr to avoid ASan false-positive
    // stack-use-after-scope when the coroutine frame is accessed cross-thread.
    auto sharedRegistry = std::make_shared<mini::net::detail::ConnectionAwaiterRegistry*>( &registry);

    struct Probe {
        std::shared_ptr<mini::net::detail::ConnectionAwaiterRegistry*> registry;
        std::shared_ptr<std::coroutine_handle<>> outHandle;

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle) {
            *outHandle = handle;
            (*registry)->armReadWaiter(handle, 1, false);
        }
        void await_resume() const noexcept {}
    };

    auto cancelledTask = [sharedRegistry, sharedState, cancelledResumePromise]() -> mini::coroutine::Task<void> {
        co_await Probe{sharedRegistry, sharedState};
        cancelledResumePromise->set_value(std::this_thread::get_id());
    }();

    std::promise<void> cancelledStarted;
    auto cancelledStartedFuture = cancelledStarted.get_future();
    loop->runInLoop([&] {
        cancelledTask.start();
        cancelledStarted.set_value();
    });
    assert(cancelledStartedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    storedHandle = *sharedState;
    assert(storedHandle);
    loop->runInLoop([&] {
        assert(!registry.cancelReadWaiter(std::noop_coroutine()));
        assert(registry.cancelReadWaiter(storedHandle));
    });
    assert(cancelledResumeFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(cancelledResumeFuture.get() == ownerThread);
    // Drain the loop to ensure cancelledTask has reached its final suspend point.
    {
        std::promise<void> drain;
        auto drainFuture = drain.get_future();
        loop->runInLoop([&drain] { drain.set_value(); });
        if (drainFuture.wait_for(std::chrono::seconds(1)) != std::future_status::ready) {
            std::abort();
        }
    }

    return 0;
}
