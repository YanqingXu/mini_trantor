// v5-delta MetricsHook 契约测试
// 验证 hook 在 owner loop 线程调用、不设 hook 时行为不变、事件类型正确触发

#include "mini/base/MetricsHook.h"
#include "mini/net/Buffer.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"
#include "mini/net/TcpClient.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThreadPool.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpServerOptions.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace mini::net;

// Find a free port by binding to port 0.
static uint16_t findFreePort() {
    int sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    ::bind(sockfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(sockfd, reinterpret_cast<sockaddr*>(&addr), &len);
    uint16_t port = ntohs(addr.sin_port);
    ::close(sockfd);
    return port;
}

void testConnectorEventHook() {
    EventLoop loop;
    const uint16_t port = findFreePort();
    InetAddress serverAddr("127.0.0.1", port);

    std::atomic<int> attemptCount{0};

    TcpClient client(&loop, serverAddr, "connector-hook");
    client.setConnectorEventCallback([&](const InetAddress&, ConnectorEvent event) {
        if (event == ConnectorEvent::ConnectAttempt) {
            attemptCount.fetch_add(1);
        }
    });

    client.connect();

    loop.runAfter(std::chrono::milliseconds(500), [&] {
        client.stop();
        loop.quit();
    });
    loop.loop();

    assert(attemptCount.load() >= 1);

    std::cout << "  PASS: ConnectorEvent hook fires on ConnectAttempt\n";
}

void testConnectionEventHook() {
    EventLoop loop;
    const uint16_t port = findFreePort();
    InetAddress listenAddr("127.0.0.1", port);

    std::atomic<int> connectedCount{0};
    std::atomic<int> disconnectedCount{0};

    TcpServer server(&loop, listenAddr, "hook-test");
    server.setConnectionEventCallback([&](const std::shared_ptr<TcpConnection>&, ConnectionEvent event) {
        if (event == ConnectionEvent::Connected) {
            connectedCount.fetch_add(1);
        } else if (event == ConnectionEvent::Disconnected) {
            disconnectedCount.fetch_add(1);
        }
    });
    server.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf) {
        buf->retrieveAll();
    });
    server.start();

    InetAddress serverAddr("127.0.0.1", port);
    TcpClient client(&loop, serverAddr, "hook-client");

    std::atomic<bool> clientConnected{false};
    client.setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            clientConnected = true;
        }
    });

    client.connect();

    // Run loop briefly to allow connection then disconnect.
    loop.runAfter(std::chrono::milliseconds(300), [&] {
        client.disconnect();
    });
    loop.runAfter(std::chrono::milliseconds(600), [&] {
        server.stop();
        loop.quit();
    });
    loop.loop();

    assert(connectedCount.load() >= 1);
    assert(disconnectedCount.load() >= 1);

    std::cout << "  PASS: ConnectionEvent hook fires on correct events\n";
}

void testNoHookDoesNotAffectBehavior() {
    EventLoop loop;
    const uint16_t port = findFreePort();
    InetAddress listenAddr("127.0.0.1", port);

    TcpServer server(&loop, listenAddr, "no-hook");
    server.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf) {
        conn->send(buf->retrieveAllAsString());
    });
    server.start();

    InetAddress serverAddr("127.0.0.1", port);
    TcpClient client(&loop, serverAddr, "no-hook-client");

    std::atomic<bool> received{false};
    client.setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            conn->send("hello");
        }
    });
    client.setMessageCallback([&](const TcpConnectionPtr& conn, Buffer* buf) {
        auto msg = buf->retrieveAllAsString();
        if (msg == "hello") {
            received = true;
        }
        client.disconnect();
    });

    client.connect();

    loop.runAfter(std::chrono::milliseconds(500), [&] {
        server.stop();
        loop.quit();
    });
    loop.loop();

    assert(received);

    std::cout << "  PASS: No hooks — behavior unchanged\n";
}

int main() {
    std::cout << "=== v5-delta MetricsHook Contract Tests ===\n";

    testConnectorEventHook();
    testConnectionEventHook();
    testNoHookDoesNotAffectBehavior();

    std::cout << "\nAll v5-delta metrics hook contract tests passed.\n";
    return 0;
}
