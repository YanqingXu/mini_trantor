#include "mini/coroutine/CancellationToken.h"
#include "mini/coroutine/SleepAwaitable.h"
#include "mini/coroutine/Task.h"
#include "mini/coroutine/WhenAny.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/NetError.h"

#include <cassert>
#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;

namespace {

mini::coroutine::Task<void> tokenBoundSleep(
    mini::net::EventLoop* loop,
    std::chrono::milliseconds delay,
    std::promise<mini::net::Expected<void>>* result,
    std::promise<std::thread::id>* resumedOn,
    mini::coroutine::CancellationToken token) {
    auto sleepResult = co_await mini::coroutine::asyncSleep(loop, delay, std::move(token));
    result->set_value(sleepResult);
    resumedOn->set_value(std::this_thread::get_id());
}

mini::coroutine::Task<int> cancellableSleepTask(
    mini::net::EventLoop* loop,
    int value,
    std::chrono::milliseconds delay,
    std::promise<mini::net::NetError>* cancelled) {
    auto result = co_await mini::coroutine::asyncSleep(loop, delay);
    if (!result) {
        cancelled->set_value(result.error());
        co_return -1;
    }
    co_return value;
}

}  // namespace

int main() {
    // Contract 1: cross-thread source.cancel() resumes SleepAwaitable on owner loop with Cancelled
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::coroutine::CancellationSource source;
        const auto callerThread = std::this_thread::get_id();

        std::promise<mini::net::Expected<void>> result;
        auto resultFuture = result.get_future();
        std::promise<std::thread::id> resumedOn;
        auto resumedOnFuture = resumedOn.get_future();

        auto task = tokenBoundSleep(loop, 10s, &result, &resumedOn, source.token());
        loop->runInLoop([&task] { task.start(); });

        std::this_thread::sleep_for(50ms);
        std::thread canceller([source] { source.cancel(); });

        assert(resultFuture.wait_for(2s) == std::future_status::ready);
        assert(resumedOnFuture.wait_for(2s) == std::future_status::ready);
        const auto sleepResult = resultFuture.get();
        assert(!sleepResult);
        assert(sleepResult.error() == mini::net::NetError::Cancelled);
        assert(resumedOnFuture.get() != callerThread);

        loop->runInLoop([loop] { loop->quit(); });
        canceller.join();
    }

    // Contract 2: WhenAny cancels losing sleep task through injected cancellation tokens
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<mini::coroutine::WhenAnyResult<int>> winner;
        auto winnerFuture = winner.get_future();
        std::promise<mini::net::NetError> loserCancelled;
        auto loserCancelledFuture = loserCancelled.get_future();

        loop->runInLoop([loop, &winner, &loserCancelled] {
            [](mini::net::EventLoop* loop,
               std::promise<mini::coroutine::WhenAnyResult<int>>* winner,
               std::promise<mini::net::NetError>* loserCancelled) -> mini::coroutine::Task<void> {
                auto result = co_await mini::coroutine::whenAny(
                    cancellableSleepTask(loop, 1, 500ms, loserCancelled),
                    cancellableSleepTask(loop, 2, 30ms, loserCancelled));
                winner->set_value(result);
                co_await mini::coroutine::asyncSleep(loop, 50ms);
                loop->quit();
            }(loop, &winner, &loserCancelled).detach();
        });

        assert(winnerFuture.wait_for(3s) == std::future_status::ready);
        assert(loserCancelledFuture.wait_for(3s) == std::future_status::ready);
        const auto result = winnerFuture.get();
        assert(result.index == 1);
        assert(result.value == 2);
        assert(loserCancelledFuture.get() == mini::net::NetError::Cancelled);
    }

    return 0;
}
