// Integration tests for HttpServer — full HTTP round-trip with real sockets.
// Tests: GET/POST requests, response body, multi-threaded server.

#include "mini/http/HttpServer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <future>
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

}  // namespace

int main() {
    // Integration 1: Full GET round-trip with path routing
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::http::HttpServer server(&loop, mini::net::InetAddress(port, true), "integ_http");

        std::promise<void> done;
        auto doneFuture = done.get_future();

        server.setHttpCallback([&](const mini::http::HttpRequest& req, mini::http::HttpResponse* resp) {
            resp->setStatusCode(mini::http::HttpResponse::k200Ok);
            resp->setStatusMessage("OK");
            resp->setContentType("text/plain");
            resp->setBody("path=" + req.path());
            resp->setCloseConnection(true);
            done.set_value();
        });
        server.start();

        std::thread client([port] {
            int fd = connectTo(port);
            std::string req = "GET /hello/world HTTP/1.1\r\n"
                              "Host: localhost\r\n"
                              "Connection: close\r\n"
                              "\r\n";
            ::write(fd, req.data(), req.size());
            std::string resp = readAll(fd);
            assert(resp.find("HTTP/1.1 200 OK") != std::string::npos);
            assert(resp.find("Content-Type: text/plain") != std::string::npos);
            assert(resp.find("path=/hello/world") != std::string::npos);
            ::close(fd);
        });

        std::thread timer([&] {
            assert(doneFuture.wait_for(3s) == std::future_status::ready);
            std::this_thread::sleep_for(100ms);
            loop.queueInLoop([&loop] { loop.quit(); });
        });

        loop.loop();
        client.join();
        timer.join();
        std::printf("  PASS: full GET round-trip\n");
    }

    // Integration 2: POST with body echoed back
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::http::HttpServer server(&loop, mini::net::InetAddress(port, true), "integ_post");

        std::promise<void> done;
        auto doneFuture = done.get_future();

        server.setHttpCallback([&](const mini::http::HttpRequest& req, mini::http::HttpResponse* resp) {
            assert(req.method() == mini::http::HttpMethod::kPost);
            resp->setStatusCode(mini::http::HttpResponse::k200Ok);
            resp->setStatusMessage("OK");
            resp->setContentType("text/plain");
            resp->setBody("echo:" + req.body());
            resp->setCloseConnection(true);
            done.set_value();
        });
        server.start();

        std::thread client([port] {
            int fd = connectTo(port);
            std::string body = "hello from client";
            std::string req = "POST /echo HTTP/1.1\r\n"
                              "Host: localhost\r\n"
                              "Content-Length: " + std::to_string(body.size()) + "\r\n"
                              "Connection: close\r\n"
                              "\r\n" + body;
            ::write(fd, req.data(), req.size());
            std::string resp = readAll(fd);
            assert(resp.find("echo:hello from client") != std::string::npos);
            ::close(fd);
        });

        std::thread timer([&] {
            assert(doneFuture.wait_for(3s) == std::future_status::ready);
            std::this_thread::sleep_for(100ms);
            loop.queueInLoop([&loop] { loop.quit(); });
        });

        loop.loop();
        client.join();
        timer.join();
        std::printf("  PASS: POST with body round-trip\n");
    }

    // Integration 3: HTTP/1.0 request with Connection: close default
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::http::HttpServer server(&loop, mini::net::InetAddress(port, true), "integ_http10");

        std::promise<void> done;
        auto doneFuture = done.get_future();

        server.setHttpCallback([&](const mini::http::HttpRequest& req, mini::http::HttpResponse* resp) {
            assert(req.version() == mini::http::HttpVersion::kHttp10);
            resp->setStatusCode(mini::http::HttpResponse::k200Ok);
            resp->setStatusMessage("OK");
            resp->setBody("http10");
            done.set_value();
        });
        server.start();

        std::thread client([port] {
            int fd = connectTo(port);
            // HTTP/1.0 without Connection header → default close
            std::string req = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
            ::write(fd, req.data(), req.size());
            std::string resp = readAll(fd);
            assert(resp.find("HTTP/1.1 200 OK") != std::string::npos);
            assert(resp.find("http10") != std::string::npos);
            assert(resp.find("Connection: close") != std::string::npos);
            ::close(fd);
        });

        std::thread timer([&] {
            assert(doneFuture.wait_for(3s) == std::future_status::ready);
            std::this_thread::sleep_for(100ms);
            loop.queueInLoop([&loop] { loop.quit(); });
        });

        loop.loop();
        client.join();
        timer.join();
        std::printf("  PASS: HTTP/1.0 close behavior\n");
    }

    std::printf("All HttpServer integration tests passed.\n");
    return 0;
}
