// DNS integration tests.
//
// 1. TcpClient connecting by hostname through DnsResolver
// 2. Coroutine-based resolve + connect chain

#include "mini/coroutine/ResolveAwaitable.h"
#include "mini/coroutine/CancellationToken.h"
#include "mini/coroutine/SleepAwaitable.h"
#include "mini/coroutine/Task.h"
#include "mini/net/Buffer.h"
#include "mini/net/DnsResolver.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/NetError.h"
#include "mini/net/TcpClient.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <future>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

uint16_t allocateTestPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    const int bound = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    assert(bound == 0);

    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    const int named = ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    assert(named == 0);

    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

void startEchoServer(mini::net::EventLoop* /*loop*/, mini::net::TcpServer& server) {
    server.setMessageCallback(
        [](const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
            const std::string msg = buf->retrieveAllAsString();
            conn->send(msg);
        });
    server.start();
}

}  // namespace

int main() {
    // Integration 1: TcpClient with hostname connects to local echo server
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        const uint16_t port = allocateTestPort();

        auto server = std::make_unique<mini::net::TcpServer>(
            loop, mini::net::InetAddress(port, true), "dns_echo_srv");
        loop->runInLoop([&] { startEchoServer(loop, *server); });
        std::this_thread::sleep_for(50ms);

        // Create TcpClient with hostname "localhost" instead of InetAddress.
        auto client = std::make_unique<mini::net::TcpClient>(
            loop, std::string("localhost"), port, "dns_client");

        std::promise<std::string> echoPromise;
        auto echoFuture = echoPromise.get_future();
        bool echoSent = false;

        loop->runInLoop([&] {
            client->setConnectionCallback(
                [&](const mini::net::TcpConnectionPtr& conn) {
                    if (conn->connected() && !echoSent) {
                        echoSent = true;
                        conn->send("hello dns");
                    }
                });
            client->setMessageCallback(
                [&](const mini::net::TcpConnectionPtr&, mini::net::Buffer* buf) {
                    echoPromise.set_value(buf->retrieveAllAsString());
                });
            client->connect();
        });

        assert(echoFuture.wait_for(5s) == std::future_status::ready);
        assert(echoFuture.get() == "hello dns");

        // Clean up on loop thread.
        std::promise<void> cleaned;
        loop->runInLoop([&] {
            client->stop();
            client.reset();
            server.reset();
            cleaned.set_value();
            loop->quit();
        });
        cleaned.get_future().wait();
        std::printf("  PASS: TcpClient hostname connect + echo\n");
    }

    // Integration 2: Coroutine-based resolve + connect chain
    {
        const uint16_t port = allocateTestPort();

        mini::net::EventLoop loop;
        mini::net::TcpServer server(&loop, mini::net::InetAddress(port, true), "coro_dns_srv");
        startEchoServer(&loop, server);

        auto resolver = std::make_shared<mini::net::DnsResolver>(1);
        std::promise<std::string> resultPromise;
        auto resultFuture = resultPromise.get_future();

        // Coroutine that resolves hostname, then connects and echoes.
        auto coroTask = [&]() -> mini::coroutine::Task<void> {
            auto addrs = co_await mini::coroutine::asyncResolve(
                resolver, &loop, "localhost", port);
            assert(addrs);
            assert(!addrs->empty());
            assert((*addrs)[0].toIp() == "127.0.0.1");

            // Use resolved address to create TcpClient and do echo.
            mini::net::TcpClient client(&loop, (*addrs)[0], "coro_dns_client");

            std::promise<mini::net::TcpConnectionPtr> connPromise;
            auto connFuture = connPromise.get_future();
            bool notified = false;

            client.setConnectionCallback(
                [&](const mini::net::TcpConnectionPtr& conn) {
                    if (conn->connected() && !notified) {
                        notified = true;
                        connPromise.set_value(conn);
                    }
                });

            std::promise<std::string> msgPromise;
            auto msgFuture = msgPromise.get_future();
            bool msgReceived = false;

            client.setMessageCallback(
                [&](const mini::net::TcpConnectionPtr&, mini::net::Buffer* buf) {
                    if (!msgReceived) {
                        msgReceived = true;
                        msgPromise.set_value(buf->retrieveAllAsString());
                    }
                });

            client.connect();

            // Wait for connection (poll-style with asyncReadSome on the connection).
            // Use a simpler approach: yield to the loop and let callbacks fire.
            // We'll poll with SleepAwaitable.
            // For simplicity, just wait for the future (blocking is OK in this test
            // because the loop is running on this same thread via loop()).
            // Actually, we need a different approach since we're inside a coroutine
            // on the loop thread. Let's use a while-loop with co_await sleep.

            // Use SleepAwaitable to yield back to the loop.
            for (int i = 0; i < 50; ++i) {
                co_await mini::coroutine::asyncSleep(&loop, 10ms);
                if (notified) break;
            }
            assert(notified);
            auto conn = connFuture.get();
            conn->send("coro dns hello");

            for (int i = 0; i < 50; ++i) {
                co_await mini::coroutine::asyncSleep(&loop, 10ms);
                if (msgReceived) break;
            }
            assert(msgReceived);
            resultPromise.set_value(msgFuture.get());

            client.stop();
        };

        // Start coroutine and run loop.
        coroTask().detach();

        // Run loop with a timeout.
        loop.runAfter(5s, [&loop] { loop.quit(); });
        loop.loop();

        assert(resultFuture.wait_for(0s) == std::future_status::ready);
        assert(resultFuture.get() == "coro dns hello");
        std::printf("  PASS: coroutine resolve + connect chain\n");
    }

    // Integration 3: coroutine-based resolve failure is explicit
    {
        mini::net::EventLoop loop;
        auto resolver = std::make_shared<mini::net::DnsResolver>(1);

        std::promise<bool> failurePromise;
        auto failureFuture = failurePromise.get_future();

        auto coroTask = [&]() -> mini::coroutine::Task<void> {
            auto addrs = co_await mini::coroutine::asyncResolve(
                resolver, &loop, "", 80);
            failurePromise.set_value(!addrs &&
                                     addrs.error() == mini::net::NetError::ResolveFailed);
            loop.quit();
        };

        coroTask().detach();
        loop.runAfter(5s, [&loop] { loop.quit(); });
        loop.loop();

        assert(failureFuture.wait_for(0s) == std::future_status::ready);
        assert(failureFuture.get());
        std::printf("  PASS: coroutine resolve failure is explicit\n");
    }

    // Integration 4: coroutine-based resolve cancellation is explicit
    {
        mini::net::EventLoop loop;
        auto resolver = std::make_shared<mini::net::DnsResolver>(1);
        mini::coroutine::CancellationSource source;
        source.cancel();

        std::promise<bool> cancelledPromise;
        auto cancelledFuture = cancelledPromise.get_future();

        auto coroTask = [&]() -> mini::coroutine::Task<void> {
            auto addrs = co_await mini::coroutine::asyncResolve(
                resolver, &loop, "localhost", 80, source.token());
            cancelledPromise.set_value(!addrs &&
                                       addrs.error() == mini::net::NetError::Cancelled);
            loop.quit();
        };

        coroTask().detach();
        loop.runAfter(5s, [&loop] { loop.quit(); });
        loop.loop();

        assert(cancelledFuture.wait_for(0s) == std::future_status::ready);
        assert(cancelledFuture.get());
        std::printf("  PASS: coroutine resolve cancellation is explicit\n");
    }

    std::printf("All DNS integration tests passed.\n");
    return 0;
}
