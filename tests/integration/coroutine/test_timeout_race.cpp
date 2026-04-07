#include "mini/coroutine/CancellationToken.h"
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

mini::coroutine::Task<mini::net::Expected<std::string>> readExpected(
    mini::net::TcpConnectionPtr connection) {
    co_return co_await connection->asyncReadSome();
}

mini::coroutine::Task<void> timeoutRound(
    mini::net::EventLoop* loop,
    mini::net::TcpConnectionPtr connection,
    std::promise<mini::net::NetError>* result) {
    auto value = co_await mini::coroutine::withTimeout(loop, readExpected(connection), 20ms);
    assert(!value);
    result->set_value(value.error());
}

mini::coroutine::Task<void> closeRound(
    mini::net::EventLoop* loop,
    mini::net::TcpConnectionPtr connection,
    std::promise<mini::net::NetError>* result) {
    auto value = co_await mini::coroutine::withTimeout(loop, readExpected(connection), 200ms);
    assert(!value);
    result->set_value(value.error());
}

mini::coroutine::Task<void> cancelRound(
    mini::net::EventLoop* loop,
    mini::net::TcpConnectionPtr connection,
    mini::coroutine::CancellationSource source,
    std::promise<mini::net::NetError>* result) {
    auto operation = readExpected(connection);
    operation.setCancellationToken(source.token());
    auto value = co_await mini::coroutine::withTimeout(loop, std::move(operation), 200ms);
    assert(!value);
    result->set_value(value.error());
}

}  // namespace

int main() {
    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* loop = loopThread.startLoop();

    // Integration 1: repeated timeout races resolve to TimedOut without hanging.
    for (int i = 0; i < 10; ++i) {
        auto sockets = makeSocketPair();
        auto connection = std::make_shared<mini::net::TcpConnection>(
            loop,
            "socketpair#timeout-race-timeout",
            sockets[0],
            mini::net::InetAddress(),
            mini::net::InetAddress());

        std::promise<mini::net::NetError> result;
        auto future = result.get_future();

        loop->runInLoop([loop, connection, &result] {
            connection->connectEstablished();
            timeoutRound(loop, connection, &result).detach();
        });

        assert(future.wait_for(3s) == std::future_status::ready);
        assert(future.get() == mini::net::NetError::TimedOut);

        ::close(sockets[1]);
        destroyConnectionOnLoop(loop, connection);
    }

    // Integration 2: peer close beats timeout and remains distinguishable.
    for (int i = 0; i < 5; ++i) {
        auto sockets = makeSocketPair();
        auto connection = std::make_shared<mini::net::TcpConnection>(
            loop,
            "socketpair#timeout-race-close",
            sockets[0],
            mini::net::InetAddress(),
            mini::net::InetAddress());

        std::promise<mini::net::NetError> result;
        auto future = result.get_future();

        loop->runInLoop([loop, connection, &result] {
            connection->connectEstablished();
            closeRound(loop, connection, &result).detach();
        });

        std::thread closer([fd = sockets[1]] {
            std::this_thread::sleep_for(20ms);
            ::close(fd);
        });

        assert(future.wait_for(3s) == std::future_status::ready);
        const auto error = future.get();
        assert(error == mini::net::NetError::PeerClosed ||
               error == mini::net::NetError::ConnectionReset);

        destroyConnectionOnLoop(loop, connection);
        closer.join();
    }

    // Integration 3: active cancellation beats timeout and is not rewritten to TimedOut.
    for (int i = 0; i < 5; ++i) {
        auto sockets = makeSocketPair();
        auto connection = std::make_shared<mini::net::TcpConnection>(
            loop,
            "socketpair#timeout-race-cancel",
            sockets[0],
            mini::net::InetAddress(),
            mini::net::InetAddress());
        mini::coroutine::CancellationSource source;

        std::promise<mini::net::NetError> result;
        auto future = result.get_future();

        loop->runInLoop([loop, connection, source, &result] {
            connection->connectEstablished();
            cancelRound(loop, connection, source, &result).detach();
        });

        std::thread canceller([source] {
            std::this_thread::sleep_for(20ms);
            source.cancel();
        });

        assert(future.wait_for(3s) == std::future_status::ready);
        assert(future.get() == mini::net::NetError::Cancelled);

        ::close(sockets[1]);
        destroyConnectionOnLoop(loop, connection);
        canceller.join();
    }

    loop->runInLoop([loop] { loop->quit(); });
    return 0;
}
