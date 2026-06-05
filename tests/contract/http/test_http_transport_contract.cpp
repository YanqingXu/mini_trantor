// test_http_transport_contract.cpp
//
// 验证 v5-epsilon HTTP 迁移后的 transport contract：
//   T1. HttpCallback 被调用，响应通过窄接口正确发送到客户端
//   T2. Connection: close 响应触发 shutdown（客户端收到 FIN）
//   T3. 格式错误请求触发 400 + 连接关闭
//   T4. Keep-alive：同一连接上处理两个请求
//
// 设计约束：TcpServer 必须在其 owner EventLoop 线程上析构，因此使用
// unique_ptr 在 loop.quit() 之前显式析构，而 loop.loop() 在主线程执行。

#include "mini/http/HttpServer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
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
    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

int connectTo(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    assert(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    return fd;
}

std::string readAll(int fd) {
    std::string result;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        result.append(buf, static_cast<std::size_t>(n));
    }
    return result;
}

std::string readResponse(int fd) {
    std::string result;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        result.append(buf, static_cast<std::size_t>(n));
        auto headerEnd = result.find("\r\n\r\n");
        if (headerEnd == std::string::npos) continue;
        auto clPos = result.find("Content-Length: ");
        if (clPos == std::string::npos) break;
        std::size_t clStart = clPos + 16;
        auto clEnd = result.find("\r\n", clStart);
        std::size_t contentLength = std::stoull(result.substr(clStart, clEnd - clStart));
        if (result.size() >= headerEnd + 4 + contentLength) break;
    }
    return result;
}

}  // namespace

// T1: HttpCallback 被调用，响应通过窄接口正确发送到客户端
void testCallbackInvokedAndResponseSent() {
    const uint16_t port = allocateTestPort();
    mini::net::EventLoop loop;

    std::atomic<bool> callbackInvoked{false};
    std::promise<void> callbackDone;
    auto callbackFuture = callbackDone.get_future();

    auto server = std::make_unique<mini::http::HttpServer>(
        &loop, mini::net::InetAddress(port, true), "transport-t1");
    server->setHttpCallback([&](const mini::http::HttpRequest&,
                                mini::http::HttpResponse* resp) {
        callbackInvoked.store(true);
        resp->setStatusCode(mini::http::HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setBody("hello-from-adapter");
        resp->setCloseConnection(true);
        callbackDone.set_value();
    });
    server->start();

    std::thread client([port] {
        int fd = connectTo(port);
        std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        ::write(fd, req.data(), req.size());
        std::string resp = readAll(fd);
        assert(!resp.empty());
        assert(resp.find("200") != std::string::npos);
        assert(resp.find("hello-from-adapter") != std::string::npos);
        ::close(fd);
    });

    // Signal quit after callback fires, from a background thread
    std::thread stopper([&] {
        assert(callbackFuture.wait_for(3s) == std::future_status::ready);
        loop.runInLoop([&] {
            server->stop();
            server.reset();
            loop.quit();
        });
    });

    loop.loop();
    client.join();
    stopper.join();

    assert(callbackInvoked.load());
    std::printf("  PASS T1: HttpCallback invoked and response sent via adapter\n");
}

// T2: Connection: close 响应触发 shutdown（客户端收到 FIN）
void testConnectionCloseTriggersShutdown() {
    const uint16_t port = allocateTestPort();
    mini::net::EventLoop loop;

    std::atomic<int> callbackCount{0};
    std::promise<void> responseSent;
    auto sentFuture = responseSent.get_future();

    auto server = std::make_unique<mini::http::HttpServer>(
        &loop, mini::net::InetAddress(port, true), "transport-t2");
    server->setHttpCallback([&](const mini::http::HttpRequest&,
                                mini::http::HttpResponse* resp) {
        ++callbackCount;
        resp->setStatusCode(mini::http::HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setBody("bye");
        resp->setCloseConnection(true);
        responseSent.set_value();
    });
    server->start();

    std::thread client([port] {
        int fd = connectTo(port);
        std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        ::write(fd, req.data(), req.size());
        std::string resp = readAll(fd);
        assert(!resp.empty());
        assert(resp.find("200") != std::string::npos);
        ::close(fd);
    });

    std::thread stopper([&] {
        assert(sentFuture.wait_for(3s) == std::future_status::ready);
        std::this_thread::sleep_for(50ms);
        loop.runInLoop([&] {
            server->stop();
            server.reset();
            loop.quit();
        });
    });

    loop.loop();
    client.join();
    stopper.join();

    assert(callbackCount.load() == 1);
    std::printf("  PASS T2: Connection: close triggers shutdown (FIN received by client)\n");
}

// T3: 格式错误的请求触发 400 Bad Request 后连接关闭
void testMalformedRequestTriggersClose() {
    const uint16_t port = allocateTestPort();
    mini::net::EventLoop loop;

    auto server = std::make_unique<mini::http::HttpServer>(
        &loop, mini::net::InetAddress(port, true), "transport-t3");
    server->setHttpCallback([](const mini::http::HttpRequest&, mini::http::HttpResponse*) {
        assert(false && "httpCallback should not be called for malformed request");
    });
    server->start();

    std::promise<std::string> responseReceived;
    auto respFuture = responseReceived.get_future();

    std::thread client([port, &responseReceived] {
        int fd = connectTo(port);
        std::string malformed = "NOT_HTTP_AT_ALL\r\n\r\n";
        ::write(fd, malformed.data(), malformed.size());
        std::string resp = readAll(fd);
        responseReceived.set_value(resp);
        ::close(fd);
    });

    std::thread stopper([&] {
        assert(respFuture.wait_for(3s) == std::future_status::ready);
        std::string resp = respFuture.get();
        assert(!resp.empty());
        assert(resp.find("400") != std::string::npos);
        loop.runInLoop([&] {
            server->stop();
            server.reset();
            loop.quit();
        });
    });

    loop.loop();
    client.join();
    stopper.join();

    std::printf("  PASS T3: Malformed request triggers 400 and server closes connection\n");
}

// T4: Keep-alive — 两个请求在同一连接上成功处理
void testKeepAliveMultipleRequests() {
    const uint16_t port = allocateTestPort();
    mini::net::EventLoop loop;

    std::atomic<int> requestCount{0};
    std::promise<void> secondDone;
    auto secondFuture = secondDone.get_future();

    auto server = std::make_unique<mini::http::HttpServer>(
        &loop, mini::net::InetAddress(port, true), "transport-t4");
    server->setHttpCallback([&](const mini::http::HttpRequest&, mini::http::HttpResponse* resp) {
        int count = ++requestCount;
        resp->setStatusCode(mini::http::HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setBody("req" + std::to_string(count));
        if (count == 2) {
            resp->setCloseConnection(true);
            secondDone.set_value();
        }
    });
    server->start();

    std::thread client([port] {
        int fd = connectTo(port);

        std::string req1 = "GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n";
        ::write(fd, req1.data(), req1.size());
        std::string r1 = readResponse(fd);
        assert(r1.find("req1") != std::string::npos);

        std::string req2 = "GET /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        ::write(fd, req2.data(), req2.size());
        std::string r2 = readAll(fd);
        assert(r2.find("req2") != std::string::npos);

        ::close(fd);
    });

    std::thread stopper([&] {
        assert(secondFuture.wait_for(3s) == std::future_status::ready);
        loop.runInLoop([&] {
            server->stop();
            server.reset();
            loop.quit();
        });
    });

    loop.loop();
    client.join();
    stopper.join();

    assert(requestCount.load() == 2);
    std::printf("  PASS T4: Keep-alive: two requests processed on same connection\n");
}

int main() {
    testCallbackInvokedAndResponseSent();
    testConnectionCloseTriggersShutdown();
    testMalformedRequestTriggersClose();
    testKeepAliveMultipleRequests();
    return 0;
}
