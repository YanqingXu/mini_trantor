// TcpClient contract tests.
//
// Core Module Change Gate:
// 1. Which loop/thread owns this module? — owner EventLoop thread.
// 2. Who owns it and who releases it? — User creates on stack/heap; destructor
//    must run on owner loop thread.
// 3. Which callbacks may re-enter? — connectionCallback may call disconnect().
// 4. Cross-thread? — connect()/disconnect()/stop() marshal via runInLoop.
// 5. Test file? — This file.

#include "mini/net/TcpClient.h"
#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <cassert>
#include <chrono>
#include <future>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

// Helper: start a simple echo TcpServer on a given loop and port.
void startEchoServer(mini::net::EventLoop* loop, mini::net::TcpServer& server) {
    server.setMessageCallback([](const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
        const std::string msg = buf->retrieveAllAsString();
        conn->send(msg);
    });
    server.start();
}

}  // namespace

int main() {
    // Contract 1: connect to listening server establishes TcpConnection on owner loop
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        const uint16_t port = 19401;
        std::promise<void> connected;
        auto connectedFuture = connected.get_future();

        auto server = std::make_unique<mini::net::TcpServer>(
            loop, mini::net::InetAddress(port, true), "test_srv");
        auto client = std::make_unique<mini::net::TcpClient>(
            loop, mini::net::InetAddress("127.0.0.1", port), "test_client");

        loop->runInLoop([&] {
            startEchoServer(loop, *server);
        });
        std::this_thread::sleep_for(50ms);

        loop->runInLoop([&] {
            client->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
                if (conn->connected()) {
                    assert(loop->isInLoopThread());
                    connected.set_value();
                }
            });
            client->connect();
        });

        assert(connectedFuture.wait_for(2s) == std::future_status::ready);

        // Clean up on loop thread
        std::promise<void> cleaned;
        loop->runInLoop([&] {
            client->stop();
            client.reset();
            server.reset();
            cleaned.set_value();
            loop->quit();
        });
        cleaned.get_future().wait();
    }

    // Contract 2: disconnect shuts down the connection
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        const uint16_t port = 19402;

        auto server = std::make_unique<mini::net::TcpServer>(
            loop, mini::net::InetAddress(port, true), "test_srv2");
        auto client = std::make_unique<mini::net::TcpClient>(
            loop, mini::net::InetAddress("127.0.0.1", port), "test_client2");

        loop->runInLoop([&] { startEchoServer(loop, *server); });
        std::this_thread::sleep_for(50ms);

        std::promise<bool> disconnected;
        auto disconnectedFuture = disconnected.get_future();
        bool disconnectFired = false;

        loop->runInLoop([&] {
            client->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
                if (conn->connected()) {
                    client->disconnect();
                } else if (!disconnectFired) {
                    disconnectFired = true;
                    disconnected.set_value(true);
                }
            });
            client->connect();
        });

        assert(disconnectedFuture.wait_for(2s) == std::future_status::ready);
        assert(disconnectedFuture.get());

        std::promise<void> cleaned;
        loop->runInLoop([&] {
            client->stop();
            client.reset();
            server.reset();
            cleaned.set_value();
            loop->quit();
        });
        cleaned.get_future().wait();
    }

    // Contract 3: cross-thread connect marshals to owner loop
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        const auto mainThread = std::this_thread::get_id();

        const uint16_t port = 19403;

        auto server = std::make_unique<mini::net::TcpServer>(
            loop, mini::net::InetAddress(port, true), "test_srv3");
        auto client = std::make_unique<mini::net::TcpClient>(
            loop, mini::net::InetAddress("127.0.0.1", port), "test_client3");

        loop->runInLoop([&] { startEchoServer(loop, *server); });
        std::this_thread::sleep_for(50ms);

        std::promise<std::thread::id> connectedOn;
        auto connectedOnFuture = connectedOn.get_future();

        loop->runInLoop([&] {
            client->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
                if (conn->connected()) {
                    connectedOn.set_value(std::this_thread::get_id());
                }
            });
            client->connect();
        });

        assert(connectedOnFuture.wait_for(2s) == std::future_status::ready);
        assert(connectedOnFuture.get() != mainThread);

        std::promise<void> cleaned;
        loop->runInLoop([&] {
            client->stop();
            client.reset();
            server.reset();
            cleaned.set_value();
            loop->quit();
        });
        cleaned.get_future().wait();
    }

    return 0;
}
