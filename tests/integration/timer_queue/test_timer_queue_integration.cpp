// TimerQueue Integration Tests
//
// Core Module Change Gate:
// 1. Which loop/thread owns this module?
//    — TimerQueue is owned by its EventLoop; all timer state is loop-thread-only.
// 2. Who owns it and who releases it?
//    — EventLoop owns TimerQueue via unique_ptr; EventLoop destructor releases it.
// 3. Which callbacks may re-enter?
//    — Timer callbacks may call runAfter/runEvery/cancel (re-entrant add/cancel).
// 4. Which operations are allowed cross-thread, and how are they marshaled?
//    — runAfter/runEvery/runAt/cancel are cross-thread safe; they marshal via
//      runInLoop/queueInLoop to the owner loop thread.
// 5. Which test file verifies this change?
//    — This file: tests/integration/timer_queue/test_timer_queue_integration.cpp

#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;

int main() {
    // Integration 1: runAfter single-shot timer fires after specified delay
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<void> fired;
        auto firedFuture = fired.get_future();
        auto start = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point fireTime;

        loop->runAfter(50ms, [&] {
            fireTime = std::chrono::steady_clock::now();
            fired.set_value();
            loop->quit();
        });

        assert(firedFuture.wait_for(2s) == std::future_status::ready);
        const auto elapsed = fireTime - start;
        // should fire after ~50ms, allow generous tolerance
        assert(elapsed >= 40ms);
        assert(elapsed < 500ms);
    }

    // Integration 2: runEvery repeating timer fires multiple times
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::atomic<int> count{0};
        std::promise<void> done;
        auto doneFuture = done.get_future();

        mini::net::TimerId repeating = loop->runEvery(20ms, [&] {
            if (count.fetch_add(1) + 1 >= 5) {
                loop->cancel(repeating);
                done.set_value();
                loop->quit();
            }
        });

        assert(doneFuture.wait_for(2s) == std::future_status::ready);
        assert(count.load() == 5);
    }

    // Integration 3: cancel prevents a pending timer from firing
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::atomic<bool> fired{false};
        auto timerId = loop->runAfter(100ms, [&] { fired = true; });

        // cancel before it fires
        std::this_thread::sleep_for(10ms);
        loop->cancel(timerId);

        // wait past the would-fire time
        loop->runAfter(200ms, [loop] { loop->quit(); });
        std::this_thread::sleep_for(300ms);
        assert(!fired.load());
    }

    // Integration 4: cross-thread runAfter marshals to owner loop
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<std::thread::id> firedOn;
        auto firedFuture = firedOn.get_future();
        const auto callerThread = std::this_thread::get_id();

        // schedule from a separate thread (not the loop thread, not this thread)
        std::thread scheduler([loop, &firedOn] {
            loop->runAfter(20ms, [loop, &firedOn] {
                firedOn.set_value(std::this_thread::get_id());
                loop->quit();
            });
        });

        assert(firedFuture.wait_for(2s) == std::future_status::ready);
        const auto executedThread = firedFuture.get();
        // must not be the caller thread or the scheduler thread
        assert(executedThread != callerThread);
        scheduler.join();
    }

    // Integration 5: timer callback can schedule another timer (re-entrant add)
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<int> result;
        auto resultFuture = result.get_future();
        int step = 0;

        loop->runAfter(10ms, [&] {
            step = 1;
            loop->runAfter(10ms, [&] {
                step = 2;
                loop->runAfter(10ms, [&] {
                    step = 3;
                    result.set_value(step);
                    loop->quit();
                });
            });
        });

        assert(resultFuture.wait_for(2s) == std::future_status::ready);
        assert(resultFuture.get() == 3);
    }

    // Integration 6: cancel from a different thread (cross-thread cancel)
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::atomic<bool> fired{false};
        auto timerId = loop->runAfter(200ms, [&] { fired = true; });

        std::thread canceller([loop, timerId] {
            std::this_thread::sleep_for(20ms);
            loop->cancel(timerId);
        });

        loop->runAfter(350ms, [loop] { loop->quit(); });
        std::this_thread::sleep_for(500ms);
        canceller.join();
        assert(!fired.load());
    }

    return 0;
}
