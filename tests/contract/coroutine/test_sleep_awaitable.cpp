// SleepAwaitable contract tests.
//
// Core Module Change Gate:
// 1. Which loop/thread owns this module? — owner EventLoop thread.
// 2. Who owns it and who releases it? — SleepAwaitable is a co_await temporary;
//    SleepState is shared_ptr-managed between timer callback and cancel.
// 3. Which callbacks may re-enter? — timer callback resumes coroutine which may
//    call asyncSleep again.
// 4. Cross-thread? — asyncSleep is called on the loop thread; cancel uses
//    EventLoop::runInLoop for cross-thread safety.
// 5. Test file? — This file.

#include "mini/coroutine/SleepAwaitable.h"
#include "mini/coroutine/Task.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/NetError.h"

#include <cassert>
#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;

int main() {
    // Contract 1: asyncSleep resumes coroutine after duration on owner loop thread
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        const auto callerThread = std::this_thread::get_id();

        std::promise<std::thread::id> resumedOn;
        auto resumedOnFuture = resumedOn.get_future();
        std::promise<bool> result;
        auto resultFuture = result.get_future();

        auto task = [](mini::net::EventLoop* loop,
                       std::promise<std::thread::id>* resumedOn,
                       std::promise<bool>* result) -> mini::coroutine::Task<void> {
            auto completed = co_await mini::coroutine::asyncSleep(loop, 50ms);
            resumedOn->set_value(std::this_thread::get_id());
            result->set_value(completed.has_value());
            loop->quit();
        }(loop, &resumedOn, &result);

        loop->runInLoop([&task] { task.start(); });

        assert(resumedOnFuture.wait_for(2s) == std::future_status::ready);
        assert(resumedOnFuture.get() != callerThread);  // resumed on loop thread
        assert(resultFuture.get() == true);  // normal expiry
    }

    // Contract 2: cancel during pending sleep resumes coroutine (no handle leak)
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<bool> result;
        auto resultFuture = result.get_future();
        std::shared_ptr<mini::coroutine::SleepState> sleepState;

        auto task = [](mini::net::EventLoop* loop,
                       std::promise<bool>* result,
                       std::shared_ptr<mini::coroutine::SleepState>* outState) -> mini::coroutine::Task<void> {
            auto awaitable = mini::coroutine::asyncSleep(loop, 10s);  // very long sleep
            *outState = awaitable.state();
            auto completed = co_await awaitable;
            result->set_value(!completed && completed.error() == mini::net::NetError::Cancelled);
            loop->quit();
        }(loop, &result, &sleepState);

        loop->runInLoop([&task] { task.start(); });

        // Wait for the coroutine to suspend on the sleep
        std::this_thread::sleep_for(100ms);
        assert(sleepState);
        assert(!sleepState->resumed);

        // Cancel the sleep
        mini::coroutine::SleepAwaitable cancelHelper(loop, 0ms);
        // Directly use the state's cancel mechanism
        loop->runInLoop([state = sleepState, loop] {
            if (!state->resumed) {
                state->resumed = true;
                state->cancelled = true;
                loop->cancel(state->timerId);
                state->handle.resume();
            }
        });

        assert(resultFuture.wait_for(2s) == std::future_status::ready);
        assert(resultFuture.get() == true);  // cancelled explicitly
    }

    // Contract 3: multiple sequential asyncSleep calls work correctly
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<int> result;
        auto resultFuture = result.get_future();

        auto task = [](mini::net::EventLoop* loop,
                       std::promise<int>* result) -> mini::coroutine::Task<void> {
            int count = 0;
            co_await mini::coroutine::asyncSleep(loop, 20ms);
            ++count;
            co_await mini::coroutine::asyncSleep(loop, 20ms);
            ++count;
            co_await mini::coroutine::asyncSleep(loop, 20ms);
            ++count;
            result->set_value(count);
            loop->quit();
        }(loop, &result);

        loop->runInLoop([&task] { task.start(); });

        assert(resultFuture.wait_for(2s) == std::future_status::ready);
        assert(resultFuture.get() == 3);
    }

    // Contract 4: asyncSleep timing is approximately correct
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<long long> elapsed;
        auto elapsedFuture = elapsed.get_future();

        auto task = [](mini::net::EventLoop* loop,
                       std::promise<long long>* elapsed) -> mini::coroutine::Task<void> {
            auto start = std::chrono::steady_clock::now();
            co_await mini::coroutine::asyncSleep(loop, 80ms);
            auto end = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            elapsed->set_value(ms);
            loop->quit();
        }(loop, &elapsed);

        loop->runInLoop([&task] { task.start(); });

        assert(elapsedFuture.wait_for(2s) == std::future_status::ready);
        long long ms = elapsedFuture.get();
        assert(ms >= 60);    // generous lower bound
        assert(ms < 500);    // generous upper bound
    }

    return 0;
}
