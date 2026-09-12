// Intent: tcp_server.intent.md. Close notifications borrow the server, not its
// connection ownership. Teardown and callback re-entry remain on their owners.
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/Socket.h"
#include "mini/net/SocketsOps.h"
#include "mini/net/TcpClient.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <memory>

using namespace mini::net;
using namespace std::chrono_literals;

static uint16_t testPort() {
    Socket socket(sockets::createNonblockingOrDie(AF_INET));
    socket.bindAddress(InetAddress(0, true));
    return InetAddress(sockets::getLocalAddr(socket.fd())).port();
}

// Hold base dispatch until the worker has finished its complete close sequence.
// A pending removal must not keep the connection alive after the server is gone.
static void delayedRemoval() {
    EventLoop loop;
    const auto port = testPort();
    auto server = std::make_unique<TcpServer>(&loop, InetAddress(port, true), "delayed", false);
    server->setThreadNum(1);
    TcpClient client(&loop, InetAddress(port, true), "client");
    std::promise<void> closeReturned;
    auto closed = closeReturned.get_future();
    bool verified = false;
    server->setConnectionCallback([&](const TcpConnectionPtr& conn) {
        assert(conn->getLoop()->isInLoopThread());
        if (conn->connected()) {
            loop.queueInLoop([&] { client.disconnect(); });
            return;
        }
        conn->getLoop()->queueInLoop([&] { closeReturned.set_value(); });
        loop.queueInLoop([&, weak = std::weak_ptr<TcpConnection>(conn)] {
            const auto status = closed.wait_for(3s);
            assert(status == std::future_status::ready);
            server.reset();
            assert(weak.expired());
            verified = true;
            loop.quit();
        });
    });
    server->start();
    client.connect();
    loop.runAfter(5s, [] { assert(false && "delayed removal timed out"); });
    loop.loop();
    client.stop();
    assert(verified);
}

// The base loop may finish before the worker returns from Disconnected. Its
// delayed close then races with server teardown and targets a closed LoopHandle.
static void closeAfterBaseExit() {
    EventLoop loop;
    const auto port = testPort();
    auto server = std::make_unique<TcpServer>(&loop, InetAddress(port, true), "exiting", false);
    server->setThreadNum(1);
    TcpClient client(&loop, InetAddress(port, true), "client");
    std::promise<void> releaseClose;
    auto release = releaseClose.get_future();
    std::atomic<int> disconnected{0};
    server->setConnectionCallback([&](const TcpConnectionPtr& conn) {
        assert(conn->getLoop()->isInLoopThread());
        if (conn->connected()) {
            loop.queueInLoop([&] { client.disconnect(); });
            return;
        }
        ++disconnected;
        loop.quit();
        const auto status = release.wait_for(3s);
        assert(status == std::future_status::ready);
    });
    server->start();
    client.connect();
    loop.runAfter(5s, [] { assert(false && "close after exit timed out"); });
    loop.loop();
    releaseClose.set_value();
    server.reset();
    client.stop();
    assert(disconnected == 1);
}

// A destructor's synchronous disconnect callbacks may call the idempotent stop.
// The local traversal must already be detached from public bookkeeping.
static void stopReentryDuringDestruction() {
    EventLoop loop;
    const auto port = testPort();
    auto server = std::make_unique<TcpServer>(&loop, InetAddress(port, true), "reentry", false);
    server->setThreadNum(0);
    auto* borrowedServer = server.get();
    TcpClient first(&loop, InetAddress(port, true), "first");
    TcpClient second(&loop, InetAddress(port, true), "second");
    int connected = 0, disconnected = 0;
    server->setConnectionCallback([&](const TcpConnectionPtr& conn) {
        assert(loop.isInLoopThread());
        if (conn->connected()) {
            if (++connected == 2) { loop.queueInLoop([&] { server.reset(); loop.quit(); }); }
        } else {
            ++disconnected;
            assert(borrowedServer->connectionCount() == 0);
            borrowedServer->stop();
        }
    });
    server->start();
    first.connect();
    second.connect();
    loop.runAfter(5s, [] { assert(false && "destruction re-entry timed out"); });
    loop.loop();
    first.stop();
    second.stop();
    assert(connected == 2 && disconnected == 2);
}

int main(int argc, char**) {
    // Optional selection lets the unsanitized retention red test and TSan race
    // reproduction run independently; the ordinary CTest entry runs everything.
    if (argc == 1) { delayedRemoval(); stopReentryDuringDestruction(); }
    for (int i = 0; i < 100; ++i) { closeAfterBaseExit(); }
}
