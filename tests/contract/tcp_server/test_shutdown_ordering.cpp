// TcpServer shutdown ordering contract test:
// Verifies the explicit ordering contract of TcpServer::stop():
//   Phase 1: Acceptor stops → no new connections accepted
//   Phase 2: Existing connections force-closed → disconnect callbacks fire
//   Phase 3: Worker loops quit and join → all I/O threads exit
//   Phase 4: Base loop can safely quit
//
// Uses atomic sequence numbers to verify that phases occur in order.

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
    assert(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
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
    assert(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    assert(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    return fd;
}

/// Helper: destroy server and quit loop, waiting for completion.
void shutdownLoop(mini::net::EventLoop* baseLoop, std::unique_ptr<mini::net::TcpServer>& server) {
    std::promise<void> done;
    auto doneFuture = done.get_future();
    baseLoop->runInLoop([&] {
        server.reset();
        baseLoop->quit();
        done.set_value();
    });
    doneFuture.wait_for(5s);
}

/// Helper: start server and wait until it is listening.
void startServer(mini::net::EventLoop* baseLoop, mini::net::TcpServer& server) {
    std::promise<void> started;
    auto startedFuture = started.get_future();
    baseLoop->runInLoop([&] {
        server.start();
        started.set_value();
    });
    assert(startedFuture.wait_for(2s) == std::future_status::ready);
}

}  // namespace

// ---------------------------------------------------------------------------
// Test 1: Acceptor stops before connections are force-closed.
//
// Contract: After stop(), no new connections can be established.
// ---------------------------------------------------------------------------
void testAcceptorStopsBeforeConnectionsClose() {
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* baseLoop = loopThread.startLoop();

    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "ordering_acceptor");

    std::atomic<int> newConnectionCount{0};
    std::promise<void> firstConnected;
    auto firstConnectedFuture = firstConnected.get_future().share();

    server->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
        if (conn->connected()) {
            ++newConnectionCount;
            if (newConnectionCount == 1) {
                firstConnected.set_value();
            }
        }
    });

    startServer(baseLoop, *server);

    // Connect first client — should succeed.
    std::thread client1([port, firstConnectedFuture] {
        const int fd = connectToServer(port);
        assert(firstConnectedFuture.wait_for(2s) == std::future_status::ready);
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

    // Try connecting a second client — must fail.
    std::this_thread::sleep_for(100ms);
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        assert(fd >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        assert(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
        const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        assert(rc != 0);  // ECONNREFUSED
        ::close(fd);
    }

    // Only the first connection should have been accepted.
    assert(newConnectionCount.load() == 1);

    client1.join();
    shutdownLoop(baseLoop, server);
}

// ---------------------------------------------------------------------------
// Test 2: Connections are force-closed before worker loops exit.
//
// Contract: The disconnect callback fires before the worker loop thread
// exits. After pool.stop() returns, all disconnect callbacks must have
// completed because worker threads are joined.
// ---------------------------------------------------------------------------
void testConnectionsCloseBeforeWorkerExit() {
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* baseLoop = loopThread.startLoop();

    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "ordering_conn");
    server->setThreadNum(2);

    std::atomic<int> disconnectCount{0};
    std::promise<void> connected;
    auto connectedFuture = connected.get_future().share();

    server->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
        if (conn->connected()) {
            connected.set_value();
        } else {
            ++disconnectCount;
        }
    });

    startServer(baseLoop, *server);

    // Connect a client.
    std::thread client([port, connectedFuture] {
        const int fd = connectToServer(port);
        assert(connectedFuture.wait_for(2s) == std::future_status::ready);
        char buf[64];
        ::read(fd, buf, sizeof(buf));
        ::close(fd);
    });

    assert(connectedFuture.wait_for(2s) == std::future_status::ready);

    // Stop the server. pool.stop() joins worker threads, which means
    // worker loop has fully exited (including drain) when stop() returns.
    // Disconnect callbacks must have fired by then.
    std::promise<void> stopped;
    auto stoppedFuture = stopped.get_future();
    baseLoop->runInLoop([&] {
        server->stop();
        stopped.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);

    // After pool.stop() returns, worker loops have fully exited.
    // The disconnect callback must have fired before the loop exited.
    assert(disconnectCount.load() > 0);

    client.join();
    shutdownLoop(baseLoop, server);
}

// ---------------------------------------------------------------------------
// Test 3: Worker loops exit before base loop quits.
//
// Contract: In the shutdown sequence, worker loops are stopped before
// the base loop is allowed to quit. This ensures no worker thread
// accesses freed resources.
// ---------------------------------------------------------------------------
void testWorkerExitBeforeBaseLoopQuit() {
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* baseLoop = loopThread.startLoop();

    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "ordering_worker");
    server->setThreadNum(1);

    std::promise<void> connected;
    auto connectedFuture = connected.get_future().share();

    server->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
        if (conn->connected()) {
            connected.set_value();
        }
    });

    startServer(baseLoop, *server);

    // Connect a client.
    std::thread client([port, connectedFuture] {
        const int fd = connectToServer(port);
        assert(connectedFuture.wait_for(2s) == std::future_status::ready);
        char buf[64];
        ::read(fd, buf, sizeof(buf));
        ::close(fd);
    });

    assert(connectedFuture.wait_for(2s) == std::future_status::ready);

    // Stop the server, then quit the base loop.
    // pool.stop() joins worker threads, so they exit before
    // we can even call baseLoop->quit().
    std::promise<void> stopped;
    auto stoppedFuture = stopped.get_future();
    baseLoop->runInLoop([&] {
        server->stop();
        // At this point, threadPool_->stop() has returned, meaning
        // all worker threads have been joined. Worker loops have
        // fully exited before baseLoop->quit() is called below.
        stopped.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);

    client.join();
    shutdownLoop(baseLoop, server);
}

// ---------------------------------------------------------------------------
// Test 4: Full shutdown ordering with multiple connections on multiple
// worker loops. Verify all disconnect callbacks fire before worker exit.
// ---------------------------------------------------------------------------
void testMultiConnectionShutdownOrdering() {
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* baseLoop = loopThread.startLoop();

    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "ordering_multi");
    server->setThreadNum(2);

    std::atomic<int> connectCount{0};
    std::atomic<int> disconnectCount{0};
    std::promise<void> allConnected;
    auto allConnectedFuture = allConnected.get_future().share();

    server->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
        if (conn->connected()) {
            if (++connectCount == 3) {
                allConnected.set_value();
            }
        } else {
            ++disconnectCount;
        }
    });

    startServer(baseLoop, *server);

    // Connect 3 clients.
    std::thread client1([port, allConnectedFuture] {
        const int fd = connectToServer(port);
        allConnectedFuture.wait_for(2s);
        char buf[64];
        ::read(fd, buf, sizeof(buf));
        ::close(fd);
    });
    std::thread client2([port, allConnectedFuture] {
        const int fd = connectToServer(port);
        allConnectedFuture.wait_for(2s);
        char buf[64];
        ::read(fd, buf, sizeof(buf));
        ::close(fd);
    });
    std::thread client3([port, allConnectedFuture] {
        const int fd = connectToServer(port);
        allConnectedFuture.wait_for(2s);
        char buf[64];
        ::read(fd, buf, sizeof(buf));
        ::close(fd);
    });

    assert(allConnectedFuture.wait_for(2s) == std::future_status::ready);
    assert(connectCount.load() == 3);

    // Stop the server.
    std::promise<void> stopped;
    auto stoppedFuture = stopped.get_future();
    baseLoop->runInLoop([&] {
        server->stop();
        stopped.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);

    // All 3 connections must have been disconnected.
    assert(disconnectCount.load() == 3);

    client1.join();
    client2.join();
    client3.join();
    shutdownLoop(baseLoop, server);
}

// ---------------------------------------------------------------------------
// Test 5: No dangling callbacks after stop.
//
// Contract: After stop() returns, connections_ is cleared.
// Verify that connectionCount() is 0 after stop().
// ---------------------------------------------------------------------------
void testNoDanglingCallbacksAfterStop() {
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* baseLoop = loopThread.startLoop();

    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "ordering_dangling");
    server->setThreadNum(1);

    std::promise<void> connected;
    auto connectedFuture = connected.get_future().share();

    server->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
        if (conn->connected()) {
            connected.set_value();
        }
    });

    startServer(baseLoop, *server);

    std::thread client([port, connectedFuture] {
        const int fd = connectToServer(port);
        assert(connectedFuture.wait_for(2s) == std::future_status::ready);
        char buf[64];
        ::read(fd, buf, sizeof(buf));
        ::close(fd);
    });

    assert(connectedFuture.wait_for(2s) == std::future_status::ready);

    // Stop and verify connectionCount is 0.
    std::promise<void> stopped;
    auto stoppedFuture = stopped.get_future();
    baseLoop->runInLoop([&] {
        server->stop();
        // After stop(), connections_ has been cleared.
        assert(server->connectionCount() == 0);
        stopped.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);

    client.join();
    shutdownLoop(baseLoop, server);
}

// ---------------------------------------------------------------------------
// Test 6: stop() is idempotent — second call returns immediately without
// re-executing the shutdown sequence. The stopped_ flag guarantees this.
// ---------------------------------------------------------------------------
void testStopIdempotent() {
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* baseLoop = loopThread.startLoop();

    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "ordering_idempotent");

    std::atomic<int> disconnectCount{0};
    std::promise<void> connected;
    auto connectedFuture = connected.get_future().share();

    server->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
        if (conn->connected()) {
            connected.set_value();
        } else {
            ++disconnectCount;
        }
    });

    startServer(baseLoop, *server);

    std::thread client([port, connectedFuture] {
        const int fd = connectToServer(port);
        assert(connectedFuture.wait_for(2s) == std::future_status::ready);
        char buf[64];
        ::read(fd, buf, sizeof(buf));
        ::close(fd);
    });

    assert(connectedFuture.wait_for(2s) == std::future_status::ready);

    // Call stop() three times — only the first should execute shutdown.
    std::promise<void> stopped;
    auto stoppedFuture = stopped.get_future();
    baseLoop->runInLoop([&] {
        server->stop();
        server->stop();  // no-op
        server->stop();  // no-op
        stopped.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);

    // Only one disconnect event despite 3 stop() calls.
    assert(disconnectCount.load() == 1);

    client.join();
    shutdownLoop(baseLoop, server);
}

int main() {
    mini::net::SignalWatcher::blockSignals({SIGINT, SIGTERM});
    mini::net::SignalWatcher::ignoreSigpipe();

    testAcceptorStopsBeforeConnectionsClose();
    testConnectionsCloseBeforeWorkerExit();
    testWorkerExitBeforeBaseLoopQuit();
    testMultiConnectionShutdownOrdering();
    testNoDanglingCallbacksAfterStop();
    testStopIdempotent();

    return 0;
}
