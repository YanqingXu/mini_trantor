// TcpClient integration tests: echo round-trip and reconnect.
//
// Core Module Change Gate:
// 1. Which loop/thread owns this module? — owner EventLoop thread.
// 2. Who owns it and who releases it? — User owns TcpClient.
// 3. Which callbacks may re-enter? — connectionCallback may call disconnect/connect.
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

int main() {
    // Integration 1: TcpClient ↔ TcpServer echo round-trip
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        const uint16_t port = 19501;

        auto server = std::make_unique<mini::net::TcpServer>(
            loop, mini::net::InetAddress(port, true), "echo_srv");
        auto client = std::make_unique<mini::net::TcpClient>(
            loop, mini::net::InetAddress("127.0.0.1", port), "echo_client");

        std::promise<std::string> echoResult;
        auto echoResultFuture = echoResult.get_future();
        bool resultSet = false;

        loop->runInLoop([&] {
            server->setMessageCallback([](const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
                conn->send(buf->retrieveAllAsString());
            });
            server->start();

            client->setConnectionCallback([](const mini::net::TcpConnectionPtr& conn) {
                if (conn->connected()) {
                    conn->send("hello mini-trantor");
                }
            });
            client->setMessageCallback([&](const mini::net::TcpConnectionPtr&, mini::net::Buffer* buf) {
                if (!resultSet) {
                    resultSet = true;
                    echoResult.set_value(buf->retrieveAllAsString());
                }
            });
            client->connect();
        });

        assert(echoResultFuture.wait_for(2s) == std::future_status::ready);
        assert(echoResultFuture.get() == "hello mini-trantor");

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

    // Integration 2: TcpClient reconnect after server-initiated close
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        const uint16_t port = 19502;

        auto server = std::make_unique<mini::net::TcpServer>(
            loop, mini::net::InetAddress(port, true), "reconn_srv");
        auto client = std::make_unique<mini::net::TcpClient>(
            loop, mini::net::InetAddress("127.0.0.1", port), "reconn_client");

        std::promise<std::string> echoResult;
        auto echoResultFuture = echoResult.get_future();
        bool resultSet = false;
        int acceptCount = 0;

        loop->runInLoop([&] {
            server->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
                if (conn->connected()) {
                    ++acceptCount;
                    if (acceptCount == 1) {
                        conn->forceClose();
                    }
                }
            });
            server->setMessageCallback([](const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
                conn->send(buf->retrieveAllAsString());
            });
            server->start();

            client->enableRetry();
            client->setConnectionCallback([](const mini::net::TcpConnectionPtr& conn) {
                if (conn->connected()) {
                    conn->send("reconnected");
                }
            });
            client->setMessageCallback([&](const mini::net::TcpConnectionPtr&, mini::net::Buffer* buf) {
                if (!resultSet) {
                    resultSet = true;
                    echoResult.set_value(buf->retrieveAllAsString());
                }
            });
            client->connect();
        });

        assert(echoResultFuture.wait_for(5s) == std::future_status::ready);
        assert(echoResultFuture.get() == "reconnected");

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
