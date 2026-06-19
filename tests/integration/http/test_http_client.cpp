// Integration tests for HTTP client.
//
// Covers:
// 1. callback 风格 GET / POST 全链路
// 2. coroutine 风格 awaitable 流程
// 3. 请求超时返回 NetError::TimedOut

#include "mini/coroutine/Task.h"
#include "mini/http/HttpClient.h"
#include "mini/http/HttpServer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <memory>
#include <atomic>
#include <future>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;
using namespace mini::http;

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

mini::coroutine::Task<void> runCoroutineApiSequence(mini::http::HttpClient* client,
                                                   std::promise<void>* done) {
    auto r1 = co_await client->asyncRequest("GET", "/co_get", "", {}, 2000);
    assert(r1 && r1->statusCode() == HttpResponse::k200Ok);
    assert(r1->body() == "co-get");

    auto r2 = co_await client->asyncRequest("POST", "/co_post", "payload", {}, 2000);
    assert(r2 && r2->statusCode() == HttpResponse::k200Ok);
    assert(r2->body() == "co-payload");
    done->set_value();
}

}  // namespace

void testGetPostRoundTrip() {
    std::promise<void> done;
    auto doneFuture = done.get_future();

    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    auto server = std::make_unique<mini::http::HttpServer>(
        loop, mini::net::InetAddress(port, true), "http_client_integ_roundtrip");
    auto client = std::make_unique<mini::http::HttpClient>(
        loop, mini::net::InetAddress("127.0.0.1", port), "http_client_integ_roundtrip_client");
    auto* clientRaw = client.get();
    auto* serverRaw = server.get();

    loop->runInLoop([&] {
        server->setHttpCallback([&](const HttpRequest& req, HttpResponse* resp) {
            if (req.path() == "/get") {
                resp->setStatusCode(HttpResponse::k200Ok);
                resp->setBody("get-ok");
            } else if (req.path() == "/post") {
                resp->setStatusCode(HttpResponse::k200Ok);
                resp->setBody("post:" + req.body());
            } else {
                resp->setStatusCode(HttpResponse::k404NotFound);
                resp->setBody("missing");
            }
        });
        server->start();

        clientRaw->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (!conn->connected()) {
                return;
            }

            clientRaw->asyncGet("/get", {}, [&, loop](mini::net::Expected<HttpResponse> resp) {
                assert(resp);
                assert(resp->statusCode() == HttpResponse::k200Ok);
                assert(resp->body() == "get-ok");

                clientRaw->asyncPost("/post", "hello", {}, [&, loop](mini::net::Expected<HttpResponse> postResp) {
                    assert(postResp);
                    assert(postResp->statusCode() == HttpResponse::k200Ok);
                    assert(postResp->body() == "post:hello");
                    done.set_value();
                }, 2000);
            }, 2000);
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
}

void testCoroutineApi() {
    std::promise<void> done;
    auto doneFuture = done.get_future();
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    auto server = std::make_unique<mini::http::HttpServer>(
        loop, mini::net::InetAddress(port, true), "http_client_integ_coro");
    auto client = std::make_unique<mini::http::HttpClient>(
        loop, mini::net::InetAddress("127.0.0.1", port), "http_client_integ_coro_client");
    auto* clientRaw = client.get();
    auto* serverRaw = server.get();

    loop->runInLoop([&] {
        server->setHttpCallback([&](const HttpRequest& req, HttpResponse* resp) {
            if (req.path() == "/co_get") {
                resp->setStatusCode(HttpResponse::k200Ok);
                resp->setBody("co-get");
            } else if (req.path() == "/co_post") {
                resp->setStatusCode(HttpResponse::k200Ok);
                resp->setBody("co-" + req.body());
            } else {
                resp->setStatusCode(HttpResponse::k404NotFound);
                resp->setBody("missing");
            }
        });
        server->start();

        clientRaw->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (conn->connected()) {
                runCoroutineApiSequence(clientRaw, &done).detach();
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
}

void testRequestTimeout() {
    std::promise<void> done;
    auto doneFuture = done.get_future();
    const int listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(listenFd >= 0);
    int one = 1;
    ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    assert(::bind(listenFd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t addrLen = static_cast<socklen_t>(sizeof(addr));
    assert(::getsockname(listenFd, reinterpret_cast<sockaddr*>(&addr), &addrLen) == 0);
    assert(::listen(listenFd, 128) == 0);

    const uint16_t noResponsePort = ntohs(addr.sin_port);
    std::atomic<bool> keepAcceptedConnection{false};
    std::atomic<bool> stopNoResponseServer{false};
    std::thread noResponseServer([listenFd, &keepAcceptedConnection, &stopNoResponseServer] {
        int cfd = ::accept(listenFd, nullptr, nullptr);
        if (cfd >= 0) {
            keepAcceptedConnection.store(true, std::memory_order_release);
            while (!stopNoResponseServer.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            ::close(cfd);
        }
        ::close(listenFd);
    });

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();
    auto client = std::make_unique<mini::http::HttpClient>(
        loop, mini::net::InetAddress("127.0.0.1", noResponsePort), "http_client_integ_timeout_client");
    auto* clientRaw = client.get();

    loop->runInLoop([&] {
            clientRaw->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
                if (!conn->connected()) {
                    return;
                }

                clientRaw->asyncGet("/hang", {}, [&, loop](mini::net::Expected<HttpResponse> resp) {
                    assert(!resp);
                    assert(resp.error() == mini::net::NetError::TimedOut);
                    done.set_value();
                }, 200);
            });
        clientRaw->connect();
    });

    assert(doneFuture.wait_for(2s) == std::future_status::ready);
    std::promise<void> cleanupDone;
    auto cleanupFuture = cleanupDone.get_future();
    loop->runInLoop([&] {
        clientRaw->stop();
        client.reset();
        loop->quit();
        cleanupDone.set_value();
    });
    cleanupFuture.wait();
    stopNoResponseServer.store(true, std::memory_order_release);
    noResponseServer.join();
}

int main() {
    testGetPostRoundTrip();
    testCoroutineApi();
    testRequestTimeout();
    return 0;
}
