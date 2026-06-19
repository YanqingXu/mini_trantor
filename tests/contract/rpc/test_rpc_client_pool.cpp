// Contract tests for RPC connection pool.
//
// 1. 连接可见重建（on connect callback should observe disconnect/reconnect）
// 2. in-flight + queued 请求在重连后能续发完成
// 3. stop 时 fail-all in-flight/pending 请求

#include "mini/rpc/RpcConnectionPool.h"
#include "mini/rpc/RpcServer.h"
#include "mini/coroutine/SleepAwaitable.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <atomic>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

uint16_t allocateTestPort() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    assert(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

void runOnLoop(mini::net::EventLoop* loop, std::function<void()> fn) {
    std::promise<void> done;
    auto doneFuture = done.get_future();
    loop->runInLoop([&] {
        fn();
        done.set_value();
    });
    assert(doneFuture.wait_for(5s) == std::future_status::ready);
}

}  // namespace

void testConnectionRebuildVisible() {
    const uint16_t port = allocateTestPort();
    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::rpc::RpcPoolOptions options;
    options.minConnections = 1;
    options.maxConnections = 1;
    options.createOnDemand = false;
    options.connector.enableRetry = true;
    options.connector.initRetryDelay = 20ms;
    options.connector.maxRetryDelay = 20ms;

    auto server = std::make_unique<mini::rpc::RpcServer>(
        loop, mini::net::InetAddress(port, true), "rpc_pool_contract_rebuild_server");
    server->registerMethod("Echo", [](std::string_view payload,
                                     std::function<void(std::string_view)> respond,
                                     std::function<void(std::string_view)>) {
        respond(payload);
    });

    auto pool = std::make_unique<mini::rpc::RpcConnectionPool>(
        loop,
        mini::net::InetAddress("127.0.0.1", port),
        "rpc_pool_contract_rebuild",
        options);
    auto* poolRaw = pool.get();

    std::atomic<int> connectedCount{0};
    std::atomic<int> disconnectedCount{0};
    bool closeScheduled = false;
    bool rebuildSignaled = false;
    std::promise<void> responseDone;
    std::promise<void> rebuildDone;
    auto responseFuture = responseDone.get_future();
    auto rebuildFuture = rebuildDone.get_future();

    runOnLoop(loop, [&] {
        server->start();
    });

    loop->runInLoop([&] {
        poolRaw->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (conn->connected()) {
                ++connectedCount;

                if (!closeScheduled) {
                    closeScheduled = true;
                    auto connKeepAlive = conn;
                    loop->runAfter(40ms, [connKeepAlive] {
                        if (connKeepAlive && connKeepAlive->connected()) {
                            connKeepAlive->forceClose();
                        }
                    });
                }

                if (connectedCount >= 2 && !rebuildSignaled) {
                    rebuildSignaled = true;
                    rebuildDone.set_value();
                }
            } else {
                ++disconnectedCount;
            }
        });

        poolRaw->start();
        poolRaw->call("Echo", "pool_rebuild",
                      [&](const std::string& error, const std::string& resp) {
                          if (error.empty()) {
                              assert(resp == "pool_rebuild");
                          }
                          responseDone.set_value();
                      },
                      3000);
    });

    assert(responseFuture.wait_for(4s) == std::future_status::ready);
    assert(rebuildFuture.wait_for(6s) == std::future_status::ready);

    std::promise<void> cleanupDone;
    auto cleanupFuture = cleanupDone.get_future();
    loop->runInLoop([&] {
        poolRaw->stop();
        pool.reset();
        server.reset();
        loop->quit();
        cleanupDone.set_value();
    });
    cleanupFuture.wait();

    assert(connectedCount.load() >= 2);
    assert(disconnectedCount.load() >= 1);
}

void testInFlightAndQueuedResendAfterReconnect() {
    const uint16_t port = allocateTestPort();
    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::rpc::RpcPoolOptions options;
    options.minConnections = 1;
    options.maxConnections = 1;
    options.createOnDemand = false;
    options.connector.enableRetry = true;
    options.connector.initRetryDelay = 20ms;
    options.connector.maxRetryDelay = 20ms;

    auto server = std::make_unique<mini::rpc::RpcServer>(
        loop, mini::net::InetAddress(port, true), "rpc_pool_contract_resume_server");
    server->registerCoroMethod(
        "SlowEcho",
        [loop](std::string payload) -> mini::coroutine::Task<std::string> {
            co_await mini::coroutine::asyncSleep(loop, 80ms);
            co_return std::string("ok:") + payload;
        });
    runOnLoop(loop, [&] {
        server->start();
    });

    auto pool = std::make_unique<mini::rpc::RpcConnectionPool>(
        loop,
        mini::net::InetAddress("127.0.0.1", port),
        "rpc_pool_contract_resume",
        options);
    auto* poolRaw = pool.get();

    std::atomic<int> successCount{0};
    std::promise<void> done;
    auto doneFuture = done.get_future();

    bool closeScheduled = false;
    loop->runInLoop([&] {
        poolRaw->start();
        poolRaw->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (conn->connected() && !closeScheduled) {
                closeScheduled = true;
                auto connKeepAlive = conn;
                // Force close during first in-flight request.
                loop->runAfter(20ms, [connKeepAlive] {
                    if (connKeepAlive && connKeepAlive->connected()) {
                        connKeepAlive->forceClose();
                    }
                });
            }
        });

        poolRaw->call("SlowEcho", "first",
                      [&](const std::string& error, const std::string& resp) {
                          assert(error.empty());
                          assert(resp == "ok:first");
                          if (++successCount == 2) {
                              done.set_value();
                          }
                      },
                      3000);
        poolRaw->call("SlowEcho", "second",
                      [&](const std::string& error, const std::string& resp) {
                          assert(error.empty());
                          assert(resp == "ok:second");
                          if (++successCount == 2) {
                              done.set_value();
                          }
                      },
                      3000);
    });

    assert(doneFuture.wait_for(8s) == std::future_status::ready);

    std::promise<void> cleanupDone;
    auto cleanupFuture = cleanupDone.get_future();
    loop->runInLoop([&] {
        poolRaw->stop();
        pool.reset();
        server.reset();
        loop->quit();
        cleanupDone.set_value();
    });
    cleanupFuture.wait();
}

void testStopFailAllPending() {
    const uint16_t port = allocateTestPort();
    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::rpc::RpcPoolOptions options;
    options.minConnections = 1;
    options.maxConnections = 1;
    options.createOnDemand = false;
    options.connector.enableRetry = true;

    auto server = std::make_unique<mini::rpc::RpcServer>(
        loop, mini::net::InetAddress(port, true), "rpc_pool_contract_stop_server");
    server->registerCoroMethod(
        "Slow",
        [loop](std::string payload) -> mini::coroutine::Task<std::string> {
            co_await mini::coroutine::asyncSleep(loop, 200ms);
            co_return payload;
        });
    runOnLoop(loop, [&] {
        server->start();
    });

    auto pool = std::make_unique<mini::rpc::RpcConnectionPool>(
        loop,
        mini::net::InetAddress("127.0.0.1", port),
        "rpc_pool_contract_stop",
        options);
    auto* poolRaw = pool.get();

    std::atomic<int> failCount{0};
    std::promise<void> done;
    auto doneFuture = done.get_future();

    loop->runInLoop([&] {
        poolRaw->start();
        poolRaw->call("Slow", "x",
                      [&](const std::string& error, const std::string&) {
                          assert(!error.empty());
                          if (++failCount == 2) {
                              done.set_value();
                          }
                      },
                      5000);
        poolRaw->call("Slow", "y",
                      [&](const std::string& error, const std::string&) {
                          assert(!error.empty());
                          if (++failCount == 2) {
                              done.set_value();
                          }
                      },
                      5000);
        loop->runAfter(50ms, [poolRaw] { poolRaw->stop(); });
    });

    assert(doneFuture.wait_for(4s) == std::future_status::ready);

    std::promise<void> cleanupDone;
    auto cleanupFuture = cleanupDone.get_future();
    loop->runInLoop([&] {
        poolRaw->stop();
        pool.reset();
        server.reset();
        loop->quit();
        cleanupDone.set_value();
    });
    cleanupFuture.wait();

    assert(failCount.load() == 2);
}

int main() {
    testConnectionRebuildVisible();
    testInFlightAndQueuedResendAfterReconnect();
    testStopFailAllPending();
    std::printf("All RPC connection pool contract tests passed.\n");
    return 0;
}
