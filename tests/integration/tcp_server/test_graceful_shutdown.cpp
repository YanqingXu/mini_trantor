// Graceful shutdown integration test:
// Verifies the ordered shutdown sequence:
//   1. Acceptor stops accepting new connections
//   2. Existing connections are force-closed
//   3. Worker loops exit
//   4. Base loop exits
//
// Also verifies:
//   - SIGINT via SignalWatcher triggers graceful shutdown
//   - TcpServer::stop() is idempotent
//   - No pending callbacks target destroyed objects

#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/SignalWatcher.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <future>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

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

int connectToServer(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const int converted = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    assert(converted == 1);

    const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    assert(rc == 0);
    return fd;
}

/// Helper: destroy server and quit loop, waiting for the loop to finish.
/// This must be called from the main thread; the server destruction and
/// loop quit are scheduled on the loop thread via runInLoop.
void shutdownLoop(mini::net::EventLoop* baseLoop, std::unique_ptr<mini::net::TcpServer>& server) {
    std::promise<void> done;
    auto doneFuture = done.get_future();
    baseLoop->runInLoop([&] {
        server.reset();
        baseLoop->quit();
        done.set_value();
    });
    // Wait until the loop thread has processed the shutdown.
    // Even though quit() makes loop() exit, the runInLoop callback
    // completes before loop() returns.
    doneFuture.wait_for(5s);
}

}  // namespace

// Test 1: TcpServer::stop() shuts down gracefully in single-thread mode.
void testSingleThreadStop() {
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* baseLoop = loopThread.startLoop();

    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "stop_test_single");

    std::atomic<int> connectionEvents{0};
    std::promise<void> connected;
    auto connectedFuture = connected.get_future().share();

    server->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
        ++connectionEvents;
        if (conn->connected()) {
            connected.set_value();
        }
    });

    std::promise<void> started;
    auto startedFuture = started.get_future();
    baseLoop->runInLoop([&] {
        server->start();
        started.set_value();
    });
    assert(startedFuture.wait_for(2s) == std::future_status::ready);

    // Connect a client.
    std::thread client([port, connectedFuture] {
        const int fd = connectToServer(port);
        assert(connectedFuture.wait_for(2s) == std::future_status::ready);
        // Keep the connection open until the server force-closes it.
        char buf[64];
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        assert(n == 0);
        ::close(fd);
    });

    // Wait for the connection to be established, then stop.
    assert(connectedFuture.wait_for(2s) == std::future_status::ready);

    // Schedule stop + destroy on the base loop.
    baseLoop->runInLoop([&] {
        server->stop();
    });

    client.join();
    assert(connectionEvents >= 2);  // connected + disconnected

    shutdownLoop(baseLoop, server);
}

// Test 2: TcpServer::stop() with worker threads.
void testMultiThreadStop() {
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* baseLoop = loopThread.startLoop();

    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "stop_test_multi");
    server->setThreadNum(2);

    std::atomic<int> connectionEvents{0};
    std::promise<void> connected;
    auto connectedFuture = connected.get_future().share();

    server->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
        ++connectionEvents;
        if (conn->connected()) {
            connected.set_value();
        }
    });

    std::promise<void> started;
    auto startedFuture = started.get_future();
    baseLoop->runInLoop([&] {
        server->start();
        started.set_value();
    });
    assert(startedFuture.wait_for(2s) == std::future_status::ready);

    // Connect a client.
    std::thread client([port, connectedFuture] {
        const int fd = connectToServer(port);
        assert(connectedFuture.wait_for(2s) == std::future_status::ready);
        // Wait for server force-close.
        char buf[64];
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        assert(n == 0);
        ::close(fd);
    });

    assert(connectedFuture.wait_for(2s) == std::future_status::ready);

    baseLoop->runInLoop([&] {
        server->stop();
    });

    client.join();
    assert(connectionEvents >= 2);

    shutdownLoop(baseLoop, server);
}

// Test 3: TcpServer::stop() is idempotent.
void testStopIdempotent() {
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* baseLoop = loopThread.startLoop();

    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "stop_idempotent");

    std::promise<void> started;
    auto startedFuture = started.get_future();
    baseLoop->runInLoop([&] {
        server->start();
        started.set_value();
    });
    assert(startedFuture.wait_for(2s) == std::future_status::ready);

    std::promise<void> done;
    auto doneFuture = done.get_future();

    baseLoop->runInLoop([&] {
        server->stop();
        server->stop();  // Second call should be safe.
        server->stop();  // Third call should be safe.
        done.set_value();
    });

    assert(doneFuture.wait_for(2s) == std::future_status::ready);

    shutdownLoop(baseLoop, server);
}

// Test 4: SIGINT via SignalWatcher triggers graceful shutdown.
// Requires blockSignals() to be called on the main thread before
// any threads are created.
void testSignalShutdown() {
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* baseLoop = loopThread.startLoop();

    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "signal_shutdown");
    server->setThreadNum(1);

    std::atomic<bool> shutdownCalled{false};
    std::promise<void> signalHandled;
    auto signalHandledFuture = signalHandled.get_future();

    // SignalWatcher must be created on the loop thread.
    baseLoop->runInLoop([&] {
        auto signalWatcher = std::make_shared<mini::net::SignalWatcher>(baseLoop, std::vector<int>{SIGINT, SIGTERM});
        signalWatcher->setSignalCallback([&, signalWatcher](int signum) {
            assert(signum == SIGINT);
            shutdownCalled = true;
            server->stop();
            server.reset();
            // Defer destruction of the SignalWatcher to after the current
            // event handling completes. Deleting it here would destroy the
            // Channel whose handleEvent is still on the call stack.
            // The lambda captures signalWatcher by value (shared_ptr copy),
            // forming a reference cycle: SignalWatcher -> callback_ -> lambda
            // -> shared_ptr<SignalWatcher>.  We break the cycle by clearing
            // the callback in the deferred task, which drops the lambda and
            // thus the last shared_ptr reference.
            baseLoop->queueInLoop([signalWatcher] {
                signalWatcher->setSignalCallback({});
                // After this, the lambda that captured signalWatcher is destroyed,
                // dropping the last reference. signalWatcher is destroyed here.
            });
            baseLoop->quit();
            signalHandled.set_value();
        });

        server->start();
    });

    // Send SIGINT from another thread.
    // This thread inherited the blocked signal mask from main().
    // Use kill() instead of raise() to send a process-directed signal
    // that signalfd can receive.
    std::thread sender([] {
        std::this_thread::sleep_for(100ms);
        ::kill(::getpid(), SIGINT);
    });

    assert(signalHandledFuture.wait_for(3s) == std::future_status::ready);
    sender.join();

    assert(shutdownCalled);
}

// Test 5: After stop(), new connections are rejected.
void testAcceptStopsAfterStop() {
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* baseLoop = loopThread.startLoop();

    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "accept_stop_test");

    std::atomic<int> connectionEvents{0};
    std::promise<void> firstConnected;
    auto firstConnectedFuture = firstConnected.get_future().share();

    server->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
        ++connectionEvents;
        if (conn->connected() && connectionEvents == 1) {
            firstConnected.set_value();
        }
    });

    std::promise<void> started;
    auto startedFuture = started.get_future();
    baseLoop->runInLoop([&] {
        server->start();
        started.set_value();
    });
    assert(startedFuture.wait_for(2s) == std::future_status::ready);

    // Connect first client (should succeed).
    std::thread client1([port, firstConnectedFuture] {
        const int fd = connectToServer(port);
        assert(firstConnectedFuture.wait_for(2s) == std::future_status::ready);
        // Wait for server force-close.
        char buf[64];
        ::read(fd, buf, sizeof(buf));
        ::close(fd);
    });

    assert(firstConnectedFuture.wait_for(2s) == std::future_status::ready);

    // Stop the server.
    std::promise<void> stopped;
    auto stoppedFuture = stopped.get_future();
    baseLoop->runInLoop([&] {
        server->stop();
        stopped.set_value();
    });

    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);

    // Give the stop a moment to take effect.
    std::this_thread::sleep_for(100ms);

    // Try connecting a second client (should fail or be refused).
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        assert(fd >= 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        const int converted = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        assert(converted == 1);

        const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        // Connection should fail (ECONNREFUSED) because the server is no longer listening.
        assert(rc != 0);
        ::close(fd);
    }

    client1.join();

    shutdownLoop(baseLoop, server);
}

int main() {
    // Block SIGINT/SIGTERM on the main thread BEFORE creating any worker threads.
    // All subsequently created threads will inherit this blocked signal mask,
    // ensuring that signals are delivered to signalfd instead of via default
    // handlers that would terminate the process.
    mini::net::SignalWatcher::blockSignals({SIGINT, SIGTERM});
    mini::net::SignalWatcher::ignoreSigpipe();

    testSingleThreadStop();
    testMultiThreadStop();
    testStopIdempotent();
    testSignalShutdown();
    testAcceptStopsAfterStop();
    return 0;
}
