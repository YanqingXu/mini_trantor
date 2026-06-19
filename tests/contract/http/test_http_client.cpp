// Contract tests for HTTP client.
//
// 1. onConnection/request callbacks must run on owner EventLoop thread.
// 2. keep-alive 复用下两个串行请求能在同一连接上完成。
// 3. 服务端 Connection: close 后，client 应自动重连后继续发起下一请求。

#include "mini/http/HttpClient.h"
#include "mini/http/HttpServer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"

#include <cassert>
#include <chrono>
#include <arpa/inet.h>
#include <future>
#include <memory>
#include <netinet/in.h>
#include <iostream>
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

}  // namespace

void testCallbacksRunOnOwnerLoop() {
    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();
    const uint16_t port = allocateTestPort();
    std::promise<void> done;
    auto doneFuture = done.get_future();

    auto server = std::make_unique<mini::http::HttpServer>(
        loop, mini::net::InetAddress(port, true), "http_client_contract_owner_loop");

    auto client = std::make_unique<mini::http::HttpClient>(
        loop, mini::net::InetAddress("127.0.0.1", port), "http_client_contract_owner_loop_client");
    auto* clientRaw = client.get();
    auto* serverRaw = server.get();

    loop->runInLoop([&] {
        server->setHttpCallback([&](const mini::http::HttpRequest&, mini::http::HttpResponse* resp) {
            resp->setStatusCode(mini::http::HttpResponse::k200Ok);
            resp->setBody("ok");
        });
        server->start();

        clientRaw->setConnectionCallback([loop,
                                         clientRaw,
                                         &done](const mini::net::TcpConnectionPtr& conn) {
            loop->assertInLoopThread();
            if (!conn->connected()) {
                return;
            }

            clientRaw->asyncGet(
                "/",
                {},
                [loop, &done](mini::net::Expected<mini::http::HttpResponse> resp) {
                    loop->assertInLoopThread();
                    assert(resp);
                    assert(resp->statusCode() == mini::http::HttpResponse::k200Ok);
                    assert(resp->body() == "ok");
                    done.set_value();
                },
                2000);
        });

        clientRaw->connect();
    });

    assert(doneFuture.wait_for(3s) == std::future_status::ready);
    {
        std::promise<void> cleanupDone;
        auto cleanupFuture = cleanupDone.get_future();
        loop->runInLoop([&] {
            clientRaw->stop();
            serverRaw->stop();
            client.reset();
            server.reset();
            loop->quit();
            cleanupDone.set_value();
        });
        cleanupFuture.wait();
    }
}

void testKeepAliveTwoRequestsInOneConnection() {
    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();
    const uint16_t port = allocateTestPort();
    std::promise<void> done;
    auto doneFuture = done.get_future();

    auto server = std::make_unique<mini::http::HttpServer>(
        loop, mini::net::InetAddress(port, true), "http_client_contract_keepalive");
    auto client = std::make_unique<mini::http::HttpClient>(
        loop, mini::net::InetAddress("127.0.0.1", port), "http_client_contract_keepalive_client");
    auto* clientRaw = client.get();
    auto* serverRaw = server.get();

    int requestCount = 0;
    int connectedCount = 0;

    loop->runInLoop([&] {
        server->setHttpCallback([&](const mini::http::HttpRequest&,
                                    mini::http::HttpResponse* resp) {
            const auto idx = ++requestCount;
            resp->setStatusCode(mini::http::HttpResponse::k200Ok);
            resp->setBody("r" + std::to_string(idx));
            resp->setCloseConnection(false);
        });
        server->start();

        clientRaw->setConnectionCallback([loop,
                                           clientRaw,
                                           &requestCount,
                                           &connectedCount,
                                           &done](const mini::net::TcpConnectionPtr& conn) {
            loop->assertInLoopThread();
            if (!conn->connected()) {
                return;
            }
            ++connectedCount;

            clientRaw->asyncGet(
                "/first",
                {},
                [loop,
                 clientRaw,
                 &requestCount,
                 &done](mini::net::Expected<mini::http::HttpResponse> resp) {
                    loop->assertInLoopThread();
                    assert(resp);
                    assert(resp->statusCode() == mini::http::HttpResponse::k200Ok);
                    assert(resp->body() == "r1");

                    clientRaw->asyncGet(
                        "/second",
                        {},
                            [clientRaw,
                             loop,
                             &done](mini::net::Expected<mini::http::HttpResponse> resp2) {
                            loop->assertInLoopThread();
                            if (!resp2) {
                                done.set_value();
                                return;
                            }
                            assert(resp2->statusCode() == mini::http::HttpResponse::k200Ok);
                            assert(resp2->body() == "r2");
                            done.set_value();
                        },
                        2000);
                },
                2000);
        });

        clientRaw->connect();
    });

    assert(doneFuture.wait_for(3s) == std::future_status::ready);
    std::promise<void> cleanupDone;
    auto cleanupFuture = cleanupDone.get_future();
    loop->runInLoop([&] {
        clientRaw->stop();
        serverRaw->stop();
        client.reset();
        server.reset();
        loop->quit();
        cleanupDone.set_value();
    });
    cleanupFuture.wait();
    assert(requestCount == 2);
    assert(connectedCount == 1);
}

void testReconnectAfterServerClose() {
    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();
    const uint16_t port = allocateTestPort();
    std::promise<void> done;
    auto doneFuture = done.get_future();

    auto server = std::make_unique<mini::http::HttpServer>(
        loop, mini::net::InetAddress(port, true), "http_client_contract_reconnect");
    auto client = std::make_unique<mini::http::HttpClient>(
        loop, mini::net::InetAddress("127.0.0.1", port), "http_client_contract_reconnect_client");
    auto* clientRaw = client.get();
    auto* serverRaw = server.get();

    int requestCount = 0;
    int connectedCount = 0;
    bool firstDispatched = false;

    loop->runInLoop([&] {
        server->setHttpCallback([&](const mini::http::HttpRequest&,
                                    mini::http::HttpResponse* resp) {
            const auto idx = ++requestCount;
            resp->setStatusCode(mini::http::HttpResponse::k200Ok);
            resp->setBody("ok-" + std::to_string(idx));
            resp->setCloseConnection(true);
        });
        server->start();

        clientRaw->setConnectionCallback([loop,
                                       clientRaw,
                                       &requestCount,
                                       &connectedCount,
                                       &done,
                                       &firstDispatched](const mini::net::TcpConnectionPtr& conn) {
            loop->assertInLoopThread();
            if (!conn->connected()) {
                return;
            }
            ++connectedCount;

            if (!firstDispatched) {
                firstDispatched = true;
                clientRaw->asyncGet(
                    "/reconnect_1",
                    {},
                    [loop,
                     clientRaw,
                     &done](mini::net::Expected<mini::http::HttpResponse> resp) {
                        loop->assertInLoopThread();
                        assert(resp);
                        assert(resp->body() == "ok-1");

                        clientRaw->asyncGet(
                            "/reconnect_2",
                            {},
                                [clientRaw,
                                 loop,
                                 &done](mini::net::Expected<mini::http::HttpResponse> resp2) {
                                loop->assertInLoopThread();
                                assert(resp2);
                                assert(resp2->body() == "ok-2");
                                done.set_value();
                            },
                            2000);
                    },
                    2000);
            }
        });

        clientRaw->connect();
    });

    assert(doneFuture.wait_for(3s) == std::future_status::ready);
    std::promise<void> cleanupDone;
    auto cleanupFuture = cleanupDone.get_future();
    loop->runInLoop([&] {
        clientRaw->stop();
        serverRaw->stop();
        client.reset();
        server.reset();
        loop->quit();
        cleanupDone.set_value();
    });
    cleanupFuture.wait();
    assert(requestCount == 2);
    assert(connectedCount >= 2);
}

int main() {
    testCallbacksRunOnOwnerLoop();
    testKeepAliveTwoRequestsInOneConnection();
    testReconnectAfterServerClose();
    return 0;
}
