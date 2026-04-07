// Contract tests for WhenAll / WhenAny coroutine combinators.
//
// Core Module Change Gate:
// 1. Which loop/thread owns this module? — Pure coroutine layer; no loop ownership.
//    Sub-task resume threads are determined by their internal awaitables.
// 2. Who owns it and who releases it? — Combinator awaitable is a co_await temporary;
//    shared control block is ref-counted across wrapper coroutines.
// 3. Which callbacks may re-enter? — None at combinator level; sub-task awaitables
//    handle their own re-entry rules.
// 4. Cross-thread? — Sub-tasks may run on different EventLoop threads; completion
//    counter uses atomic operations. Parent resumes on last/first completing thread.
// 5. Test file? — This file.

#include "mini/coroutine/CancellationToken.h"
#include "mini/coroutine/SleepAwaitable.h"
#include "mini/coroutine/Task.h"
#include "mini/coroutine/WhenAll.h"
#include "mini/coroutine/WhenAny.h"
#include "mini/net/NetError.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;
using mini::coroutine::Task;
using mini::coroutine::WhenAnyResult;
using mini::coroutine::whenAll;
using mini::coroutine::whenAny;

namespace {

// Helper: a task that sleeps then returns a value
Task<int> sleepAndReturn(mini::net::EventLoop* loop, int value,
                         std::chrono::milliseconds delay) {
    co_await mini::coroutine::asyncSleep(loop, delay);
    co_return value;
}

// Helper: a task that sleeps (void)
Task<void> sleepVoid(mini::net::EventLoop* loop,
                     std::chrono::milliseconds delay) {
    co_await mini::coroutine::asyncSleep(loop, delay);
    co_return;
}

// Helper: a task that records which thread it resumed on
Task<int> recordThread(mini::net::EventLoop* loop, int value,
                       std::chrono::milliseconds delay,
                       std::thread::id* threadOut) {
    co_await mini::coroutine::asyncSleep(loop, delay);
    *threadOut = std::this_thread::get_id();
    co_return value;
}

std::array<int, 2> makeSocketPair() {
    std::array<int, 2> sockets{};
    const int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data());
    assert(rc == 0);
    return sockets;
}

Task<int> readLengthOrCancelled(
    mini::net::TcpConnectionPtr connection,
    std::promise<mini::net::NetError>* loserCancelled) {
    auto result = co_await connection->asyncReadSome();
    if (!result) {
        loserCancelled->set_value(result.error());
        co_return -1;
    }
    co_return static_cast<int>(result->size());
}

Task<void> whenAnyReadTimeoutAndQuit(
    mini::net::EventLoop* loop,
    mini::net::TcpConnectionPtr connection,
    std::promise<WhenAnyResult<int>>* winner,
    std::promise<mini::net::NetError>* loserCancelled) {
    auto result = co_await whenAny(
        readLengthOrCancelled(connection, loserCancelled),
        sleepAndReturn(loop, 7, 50ms));
    winner->set_value(result);
    co_await mini::coroutine::asyncSleep(loop, 50ms);
    connection->forceClose();
    connection->connectDestroyed();
    loop->quit();
}

}  // namespace

int main() {
    // Contract 1: WhenAll with SleepAwaitable — all sub-tasks complete on loop thread
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<std::tuple<int, int>> result;
        auto resultFuture = result.get_future();

        loop->runInLoop([loop, &result] {
            [](mini::net::EventLoop* loop,
               std::promise<std::tuple<int, int>>* result) -> Task<void> {
                auto [a, b] = co_await whenAll(
                    sleepAndReturn(loop, 10, 50ms),
                    sleepAndReturn(loop, 20, 80ms));
                result->set_value({a, b});
                loop->quit();
            }(loop, &result).detach();
        });

        assert(resultFuture.wait_for(3s) == std::future_status::ready);
        auto [a, b] = resultFuture.get();
        assert(a == 10);
        assert(b == 20);
    }

    // Contract 2: WhenAny timeout pattern — fast task wins over slow task
    // Note: slow task uses 500ms (not 2s) so it also completes before loop
    // shutdown, avoiding leaked coroutine frames from abandoned timers.
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<WhenAnyResult<int>> result;
        auto resultFuture = result.get_future();

        loop->runInLoop([loop, &result] {
            [](mini::net::EventLoop* loop,
               std::promise<WhenAnyResult<int>>* result) -> Task<void> {
                auto r = co_await whenAny(
                    sleepAndReturn(loop, 999, 500ms),  // slow — should lose
                    sleepAndReturn(loop, 42, 50ms));   // fast — should win
                result->set_value(r);
                // Wait for the losing sub-task's timer to fire before quitting,
                // so its wrapper coroutine completes cleanly.
                co_await mini::coroutine::asyncSleep(loop, 600ms);
                loop->quit();
            }(loop, &result).detach();
        });

        assert(resultFuture.wait_for(3s) == std::future_status::ready);
        auto r = resultFuture.get();
        assert(r.index == 1);   // second task (fast) wins
        assert(r.value == 42);
    }

    // Contract 3: WhenAll void tasks with SleepAwaitable
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<bool> done;
        auto doneFuture = done.get_future();

        loop->runInLoop([loop, &done] {
            [](mini::net::EventLoop* loop,
               std::promise<bool>* done) -> Task<void> {
                co_await whenAll(
                    sleepVoid(loop, 30ms),
                    sleepVoid(loop, 60ms));
                done->set_value(true);
                loop->quit();
            }(loop, &done).detach();
        });

        assert(doneFuture.wait_for(3s) == std::future_status::ready);
        assert(doneFuture.get() == true);
    }

    // Contract 4: WhenAny void tasks — first to complete wins
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<std::size_t> winnerIndex;
        auto winnerFuture = winnerIndex.get_future();

        loop->runInLoop([loop, &winnerIndex] {
            [](mini::net::EventLoop* loop,
               std::promise<std::size_t>* winnerIndex) -> Task<void> {
                auto r = co_await whenAny(
                    sleepVoid(loop, 500ms),  // slow
                    sleepVoid(loop, 50ms));  // fast
                winnerIndex->set_value(r.index);
                // Wait for the loser to complete before quitting.
                co_await mini::coroutine::asyncSleep(loop, 600ms);
                loop->quit();
            }(loop, &winnerIndex).detach();
        });

        assert(winnerFuture.wait_for(3s) == std::future_status::ready);
        assert(winnerFuture.get() == 1);  // second task wins
    }

    // Contract 5: Coroutine handle leak prevention — all frames destroyed
    // We verify this indirectly: if handles leaked, ASAN/LSAN would catch it.
    // This test exercises WhenAny where loser tasks must be cleaned up.
    // Slow tasks use 200ms so they complete within the delay before loop quit.
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<bool> done;
        auto doneFuture = done.get_future();

        loop->runInLoop([loop, &done] {
            [](mini::net::EventLoop* loop,
               std::promise<bool>* done) -> Task<void> {
                // Run several WhenAny rounds to stress cleanup
                for (int i = 0; i < 5; ++i) {
                    auto r = co_await whenAny(
                        sleepAndReturn(loop, i, 200ms),
                        sleepAndReturn(loop, i + 100, 30ms));
                    assert(r.index == 1);
                    assert(r.value == i + 100);
                    // Wait for the loser's timer to fire.
                    co_await mini::coroutine::asyncSleep(loop, 250ms);
                }
                done->set_value(true);
                loop->quit();
            }(loop, &done).detach();
        });

        assert(doneFuture.wait_for(15s) == std::future_status::ready);
        assert(doneFuture.get());
    }

    // Contract 6: WhenAny(asyncReadSome, asyncSleep) cancels read loser explicitly
    {
        auto sockets = makeSocketPair();
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        auto connection = std::make_shared<mini::net::TcpConnection>(
            loop,
            "socketpair#whenany-timeout",
            sockets[0],
            mini::net::InetAddress(),
            mini::net::InetAddress());

        std::promise<WhenAnyResult<int>> winner;
        auto winnerFuture = winner.get_future();
        std::promise<mini::net::NetError> loserCancelled;
        auto loserCancelledFuture = loserCancelled.get_future();

        loop->runInLoop([loop, connection, &winner, &loserCancelled] {
            connection->connectEstablished();
            whenAnyReadTimeoutAndQuit(loop, connection, &winner, &loserCancelled).detach();
        });

        assert(winnerFuture.wait_for(3s) == std::future_status::ready);
        assert(loserCancelledFuture.wait_for(3s) == std::future_status::ready);
        auto result = winnerFuture.get();
        assert(result.index == 1);
        assert(result.value == 7);
        assert(loserCancelledFuture.get() == mini::net::NetError::Cancelled);

        ::close(sockets[1]);
    }

    return 0;
}
