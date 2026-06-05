// EventLoopThreadPool::stop() unit test:
// Verifies that stop() quits all worker loops, clears containers, resets started_
// and next_, and that getNextLoop() falls back to baseLoop after stop.
// Covers: normal stop, stop with running tasks, zero-thread stop, idempotent stop,
//         stop+restart, pending task drain, round-robin reset.
//
// Pattern: baseLoop is created on the main thread but never runs loop().
// All pool operations that require baseLoop thread are called synchronously
// on the main thread (which IS the baseLoop thread). Worker loops run loop()
// in their own threads and are properly joined during stop().

#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThreadPool.h"

#include <cassert>
#include <future>
#include <thread>

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Test 1: Normal stop — start multi-thread pool, then stop.
// All worker loops must exit, threads join, and containers clear.
// After stop, getNextLoop() returns baseLoop and getAllLoops() returns {baseLoop}.
// ---------------------------------------------------------------------------
void testNormalStop() {
    mini::net::EventLoop baseLoop;
    mini::net::EventLoopThreadPool pool(&baseLoop, "normal");

    pool.setThreadNum(3);
    std::atomic<int> initCount{0};
    pool.start([&](mini::net::EventLoop*) {
        ++initCount;
    });
    assert(initCount == 3);

    // Verify worker loops are running by scheduling tasks on them.
    auto loops = pool.getAllLoops();
    assert(loops.size() == 3);
    for (auto* loop : loops) {
        std::promise<void> taskRan;
        auto f = taskRan.get_future();
        loop->runInLoop([&] { taskRan.set_value(); });
        assert(f.wait_for(2s) == std::future_status::ready);
    }

    // Stop the pool on the baseLoop thread (which is the main thread here).
    pool.stop();

    // After stop: getAllLoops() falls back to {baseLoop}.
    assert(pool.getAllLoops().size() == 1);
    assert(pool.getAllLoops()[0] == &baseLoop);

    // getNextLoop() falls back to baseLoop.
    assert(pool.getNextLoop() == &baseLoop);
}

// ---------------------------------------------------------------------------
// Test 2: Stop while tasks are running — schedule tasks on worker loops,
// then stop. The currently executing functor completes before the loop exits,
// because EventLoop drains pending functors after quit.
// ---------------------------------------------------------------------------
void testStopWithRunningTasks() {
    mini::net::EventLoop baseLoop;
    mini::net::EventLoopThreadPool pool(&baseLoop, "running");

    pool.setThreadNum(2);
    pool.start();
    auto loops = pool.getAllLoops();
    assert(loops.size() == 2);

    // Schedule a task on each worker: signal start, sleep, signal finish.
    std::atomic<int> started{0};
    std::atomic<int> finished{0};

    for (auto* loop : loops) {
        loop->runInLoop([&] {
            ++started;
            std::this_thread::sleep_for(50ms);
            ++finished;
        });
    }

    // Wait until both tasks have started (they are executing inside
    // doPendingFunctors on the worker loops).
    while (started.load() < 2) {
        std::this_thread::sleep_for(10ms);
    }

    // Stop the pool while tasks are still sleeping.
    // pool.stop() calls loop->quit() then joins threads.
    // Each worker loop finishes the current functor, then exits.
    pool.stop();

    // After stop returns, threads are joined. The in-flight functors must have
    // completed because EventLoop finishes doPendingFunctors before checking quit_.
    assert(finished.load() == 2);
}

// ---------------------------------------------------------------------------
// Test 3: Zero-thread stop — stop() on a pool with no worker threads is a
// safe no-op.
// ---------------------------------------------------------------------------
void testZeroThreadStop() {
    mini::net::EventLoop baseLoop;
    mini::net::EventLoopThreadPool pool(&baseLoop, "zero");

    pool.start();  // zero threads
    assert(pool.getNextLoop() == &baseLoop);
    assert(pool.getAllLoops().size() == 1);
    assert(pool.getAllLoops()[0] == &baseLoop);

    // stop() should be a no-op and not crash.
    pool.stop();

    // getNextLoop still returns baseLoop.
    assert(pool.getNextLoop() == &baseLoop);
    assert(pool.getAllLoops().size() == 1);
}

// ---------------------------------------------------------------------------
// Test 4: Idempotent stop — calling stop() twice is safe.
// ---------------------------------------------------------------------------
void testStopIdempotent() {
    mini::net::EventLoop baseLoop;
    mini::net::EventLoopThreadPool pool(&baseLoop, "idempotent");

    pool.setThreadNum(2);
    pool.start();
    assert(pool.getAllLoops().size() == 2);

    // First stop.
    pool.stop();
    assert(pool.getAllLoops().size() == 1);
    assert(pool.getAllLoops()[0] == &baseLoop);

    // Second stop — should be a no-op, not crash.
    // loops_ is already empty, so the for-loop in stop() iterates zero times.
    pool.stop();
    assert(pool.getNextLoop() == &baseLoop);
}

// ---------------------------------------------------------------------------
// Test 5: Stop then restart — after stop(), start() can be called again to
// create new worker loops.
// ---------------------------------------------------------------------------
void testStopThenRestart() {
    mini::net::EventLoop baseLoop;
    mini::net::EventLoopThreadPool pool(&baseLoop, "restart");

    pool.setThreadNum(2);
    pool.start();
    assert(pool.getAllLoops().size() == 2);

    // Stop.
    pool.stop();
    assert(pool.getAllLoops().size() == 1);

    // Restart with a different thread count.
    pool.setThreadNum(1);
    pool.start();

    auto loops2 = pool.getAllLoops();
    assert(loops2.size() == 1);
    assert(loops2[0] != &baseLoop);

    // Verify the new worker loop is functional.
    std::promise<void> taskRan;
    auto f = taskRan.get_future();
    loops2[0]->runInLoop([&] { taskRan.set_value(); });
    assert(f.wait_for(2s) == std::future_status::ready);

    // Clean up.
    pool.stop();
}

// ---------------------------------------------------------------------------
// Test 6: Pending cross-thread tasks are drained — schedule tasks via
// queueInLoop on worker loops, then stop. All queued tasks should complete
// because EventLoop drains pending functors before loop() returns.
// ---------------------------------------------------------------------------
void testPendingTasksDrained() {
    mini::net::EventLoop baseLoop;
    mini::net::EventLoopThreadPool pool(&baseLoop, "drain");

    pool.setThreadNum(2);
    pool.start();
    auto loops = pool.getAllLoops();

    // Queue tasks from the main thread to each worker loop.
    std::atomic<int> taskCount{0};
    const int tasksPerLoop = 5;

    for (auto* loop : loops) {
        for (int i = 0; i < tasksPerLoop; ++i) {
            loop->queueInLoop([&] { ++taskCount; });
        }
    }

    // Give a moment for some tasks to be queued (they may execute immediately).
    std::this_thread::sleep_for(50ms);

    // Stop the pool. EventLoop drains pending functors before loop() returns,
    // so all queued tasks should complete.
    pool.stop();

    // After stop(), threads have been joined, so all loops have fully exited.
    // All pending functors should have been drained.
    assert(taskCount.load() == 2 * tasksPerLoop);
}

// ---------------------------------------------------------------------------
// Test 7: getNextLoop round-robin resets after stop+restart.
// stop() resets next_ to 0, so the first getNextLoop after restart returns
// the first new worker loop.
// ---------------------------------------------------------------------------
void testRoundRobinResetsAfterRestart() {
    mini::net::EventLoop baseLoop;
    mini::net::EventLoopThreadPool pool(&baseLoop, "rr-reset");

    pool.setThreadNum(3);
    pool.start();
    auto loops = pool.getAllLoops();
    assert(loops.size() == 3);

    // Advance next_ by consuming two getNextLoop calls.
    assert(pool.getNextLoop() == loops[0]);
    assert(pool.getNextLoop() == loops[1]);
    // next_ is now 2.

    // Stop resets next_ to 0.
    pool.stop();

    // Restart with the same thread count.
    pool.setThreadNum(3);
    pool.start();

    auto loops2 = pool.getAllLoops();
    assert(loops2.size() == 3);

    // next_ was reset, so the first getNextLoop returns loops2[0].
    assert(pool.getNextLoop() == loops2[0]);

    pool.stop();
}

// ---------------------------------------------------------------------------
// Test 8: stop() joins all worker threads — after stop() returns, no worker
// thread is running. Verify by checking that all scheduled tasks completed.
// ---------------------------------------------------------------------------
void testStopJoinsAllThreads() {
    mini::net::EventLoop baseLoop;
    mini::net::EventLoopThreadPool pool(&baseLoop, "join");

    pool.setThreadNum(4);
    pool.start();
    auto loops = pool.getAllLoops();
    assert(loops.size() == 4);

    // Schedule a task on each worker loop.
    std::atomic<int> count{0};
    for (auto* loop : loops) {
        loop->runInLoop([&] { ++count; });
    }

    // stop() must join all threads, so by the time it returns, all tasks
    // have completed (they were either already executed or drained during exit).
    pool.stop();
    assert(count.load() == 4);
}

int main() {
    testNormalStop();
    testStopWithRunningTasks();
    testZeroThreadStop();
    testStopIdempotent();
    testStopThenRestart();
    testPendingTasksDrained();
    testRoundRobinResetsAfterRestart();
    testStopJoinsAllThreads();
    return 0;
}
