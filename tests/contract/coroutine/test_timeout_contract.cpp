#include "mini/coroutine/CancellationToken.h"
#include "mini/coroutine/SleepAwaitable.h"
#include "mini/coroutine/Task.h"
#include "mini/coroutine/Timeout.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/NetError.h"
#include "mini/net/TcpConnection.h"

#include <array>
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

std::array<int, 2> makeSocketPair() {
    std::array<int, 2> sockets{};
    const int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data());
    assert(rc == 0);
    return sockets;
}

void destroyConnectionOnLoop(
    mini::net::EventLoop* loop,
    mini::net::TcpConnectionPtr connection) {
    std::promise<void> destroyed;
    auto future = destroyed.get_future();
    loop->runInLoop([connection = std::move(connection), &destroyed]() mutable {
        connection->connectDestroyed();
        connection.reset();
        destroyed.set_value();
    });
    assert(future.wait_for(1s) == std::future_status::ready);
}

mini::coroutine::Task<mini::net::Expected<int>> expectedSleepValue(
    mini::net::EventLoop* loop,
    int value,
    std::chrono::milliseconds delay) {
    auto result = co_await mini::coroutine::asyncSleep(loop, delay);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return value;
}

mini::coroutine::Task<mini::net::Expected<void>> expectedSleepVoid(
    mini::net::EventLoop* loop,
    std::chrono::milliseconds delay) {
    co_return co_await mini::coroutine::asyncSleep(loop, delay);
}

mini::coroutine::Task<mini::net::Expected<std::string>> expectedRead(
    mini::net::TcpConnectionPtr connection) {
    co_return co_await connection->asyncReadSome();
}

}  // namespace

int main() {
    // Contract 1: withTimeout returns the wrapped success result when operation wins.
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<mini::net::Expected<int>> result;
        auto resultFuture = result.get_future();

        loop->runInLoop([loop, &result] {
            [](mini::net::EventLoop* loop,
               std::promise<mini::net::Expected<int>>* result) -> mini::coroutine::Task<void> {
                result->set_value(co_await mini::coroutine::withTimeout(
                    loop,
                    expectedSleepValue(loop, 7, 20ms),
                    200ms));
                loop->quit();
            }(loop, &result).detach();
        });

        assert(resultFuture.wait_for(3s) == std::future_status::ready);
        const auto value = resultFuture.get();
        assert(value);
        assert(*value == 7);
    }

    // Contract 2: withTimeout reports TimedOut distinctly from Cancelled.
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<mini::net::Expected<int>> result;
        auto resultFuture = result.get_future();
        std::promise<bool> resumedOnLoop;
        auto resumedOnLoopFuture = resumedOnLoop.get_future();

        loop->runInLoop([loop, &result, &resumedOnLoop] {
            [](mini::net::EventLoop* loop,
               std::promise<mini::net::Expected<int>>* result,
               std::promise<bool>* resumedOnLoop) -> mini::coroutine::Task<void> {
                result->set_value(co_await mini::coroutine::withTimeout(
                    loop,
                    expectedSleepValue(loop, 99, 300ms),
                    30ms));
                resumedOnLoop->set_value(loop->isInLoopThread());
                co_await mini::coroutine::asyncSleep(loop, 50ms);
                loop->quit();
            }(loop, &result, &resumedOnLoop).detach();
        });

        assert(resultFuture.wait_for(3s) == std::future_status::ready);
        assert(resumedOnLoopFuture.wait_for(3s) == std::future_status::ready);
        const auto value = resultFuture.get();
        assert(!value);
        assert(value.error() == mini::net::NetError::TimedOut);
        assert(resumedOnLoopFuture.get());
    }

    // Contract 3: withTimeout preserves operation-level active cancellation.
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        mini::coroutine::CancellationSource source;

        auto operation = std::make_shared<mini::coroutine::Task<mini::net::Expected<int>>>(
            expectedSleepValue(loop, 1, 10s));
        operation->setCancellationToken(source.token());

        std::promise<mini::net::Expected<int>> result;
        auto resultFuture = result.get_future();

        loop->runInLoop([loop, operation, &result]() mutable {
            [](mini::net::EventLoop* loop,
               std::shared_ptr<mini::coroutine::Task<mini::net::Expected<int>>> operation,
               std::promise<mini::net::Expected<int>>* result) -> mini::coroutine::Task<void> {
                result->set_value(co_await mini::coroutine::withTimeout(
                    loop,
                    std::move(*operation),
                    500ms));
                loop->quit();
            }(loop, operation, &result).detach();
        });

        std::this_thread::sleep_for(50ms);
        std::thread canceller([source] { source.cancel(); });

        assert(resultFuture.wait_for(3s) == std::future_status::ready);
        const auto value = resultFuture.get();
        assert(!value);
        assert(value.error() == mini::net::NetError::Cancelled);
        canceller.join();
    }

    // Contract 4: withTimeout preserves peer-close semantics instead of masking them as timeout.
    {
        auto sockets = makeSocketPair();
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        auto connection = std::make_shared<mini::net::TcpConnection>(
            loop,
            "socketpair#timeout-contract-close",
            sockets[0],
            mini::net::InetAddress(),
            mini::net::InetAddress());

        std::promise<mini::net::Expected<std::string>> result;
        auto resultFuture = result.get_future();

        loop->runInLoop([loop, connection, &result] {
            connection->connectEstablished();
            [](mini::net::EventLoop* loop,
               mini::net::TcpConnectionPtr connection,
               std::promise<mini::net::Expected<std::string>>* result) -> mini::coroutine::Task<void> {
                result->set_value(co_await mini::coroutine::withTimeout(
                    loop,
                    expectedRead(connection),
                    500ms));
            }(loop, connection, &result).detach();
        });

        std::thread peerCloser([fd = sockets[1]] {
            std::this_thread::sleep_for(50ms);
            ::close(fd);
        });

        assert(resultFuture.wait_for(3s) == std::future_status::ready);
        const auto value = resultFuture.get();
        assert(!value);
        assert(value.error() == mini::net::NetError::PeerClosed ||
               value.error() == mini::net::NetError::ConnectionReset);

        destroyConnectionOnLoop(loop, connection);
        loop->runInLoop([loop] { loop->quit(); });
        peerCloser.join();
    }

    // Contract 5: void-like operations use the same TimedOut surface.
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<mini::net::Expected<void>> result;
        auto resultFuture = result.get_future();

        loop->runInLoop([loop, &result] {
            [](mini::net::EventLoop* loop,
               std::promise<mini::net::Expected<void>>* result) -> mini::coroutine::Task<void> {
                result->set_value(co_await mini::coroutine::withTimeout(
                    loop,
                    expectedSleepVoid(loop, 200ms),
                    20ms));
                co_await mini::coroutine::asyncSleep(loop, 50ms);
                loop->quit();
            }(loop, &result).detach();
        });

        assert(resultFuture.wait_for(3s) == std::future_status::ready);
        const auto value = resultFuture.get();
        assert(!value);
        assert(value.error() == mini::net::NetError::TimedOut);
    }

    return 0;
}
