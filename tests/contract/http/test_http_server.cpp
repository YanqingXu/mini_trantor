// Contract tests for HttpServer.
// Verifies: callback thread affinity, keep-alive, Connection: close, 400 on malformed.

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

std::string readResponse(int fd) {
    // Read until we get a complete HTTP response (headers + body based on Content-Length).
    std::string result;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        result.append(buf, static_cast<std::size_t>(n));

        // Check if we have complete headers
        auto headerEnd = result.find("\r\n\r\n");
        if (headerEnd == std::string::npos) continue;

        // Find Content-Length
        auto clPos = result.find("Content-Length: ");
        if (clPos == std::string::npos) break;  // No content-length, assume done after headers

        std::size_t clStart = clPos + 16;
        auto clEnd = result.find("\r\n", clStart);
        std::size_t contentLength = std::stoull(result.substr(clStart, clEnd - clStart));

        std::size_t bodyStart = headerEnd + 4;
        if (result.size() >= bodyStart + contentLength) {
            break;
        }
    }
    return result;
}

}  // namespace

int main() {
    // Contract 1: HttpCallback fires on connection's owner loop thread
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::http::HttpServer server(&loop, mini::net::InetAddress(port, true), "contract_http");

        std::promise<bool> threadOk;
        auto threadFuture = threadOk.get_future();

        server.setHttpCallback([&](const mini::http::HttpRequest& req, mini::http::HttpResponse* resp) {
            threadOk.set_value(loop.isInLoopThread());
            resp->setStatusCode(mini::http::HttpResponse::k200Ok);
            resp->setStatusMessage("OK");
            resp->setBody("ok");
            resp->setCloseConnection(true);
        });
        server.start();

        std::thread client([port] {
            int fd = connectTo(port);
            std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
            ::write(fd, req.data(), req.size());
            readAll(fd);
            ::close(fd);
        });

        std::thread timer([&] {
            assert(threadFuture.wait_for(3s) == std::future_status::ready);
            assert(threadFuture.get() == true);
            loop.queueInLoop([&loop] { loop.quit(); });
        });

        loop.loop();
        client.join();
        timer.join();
        std::printf("  PASS: HttpCallback fires on owner loop thread\n");
    }

    // Contract 2: Keep-alive — two requests on same connection
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::http::HttpServer server(&loop, mini::net::InetAddress(port, true), "contract_keepalive");

        std::atomic<int> requestCount{0};
        std::promise<void> secondDone;
        auto secondFuture = secondDone.get_future();

        server.setHttpCallback([&](const mini::http::HttpRequest& req, mini::http::HttpResponse* resp) {
            int count = ++requestCount;
            resp->setStatusCode(mini::http::HttpResponse::k200Ok);
            resp->setStatusMessage("OK");
            resp->setBody("req" + std::to_string(count));
            if (count == 2) {
                resp->setCloseConnection(true);
                secondDone.set_value();
            }
        });
        server.start();

        std::thread client([port] {
            int fd = connectTo(port);

            // First request
            std::string req1 = "GET /first HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
            ::write(fd, req1.data(), req1.size());
            std::string resp1 = readResponse(fd);
            assert(resp1.find("HTTP/1.1 200 OK") != std::string::npos);
            assert(resp1.find("req1") != std::string::npos);

            // Second request on same connection
            std::string req2 = "GET /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
            ::write(fd, req2.data(), req2.size());
            std::string resp2 = readAll(fd);
            assert(resp2.find("HTTP/1.1 200 OK") != std::string::npos);
            assert(resp2.find("req2") != std::string::npos);

            ::close(fd);
        });

        std::thread timer([&] {
            assert(secondFuture.wait_for(3s) == std::future_status::ready);
            // Give time for shutdown to complete
            std::this_thread::sleep_for(100ms);
            loop.queueInLoop([&loop] { loop.quit(); });
        });

        loop.loop();
        client.join();
        timer.join();
        assert(requestCount.load() == 2);
        std::printf("  PASS: keep-alive processes two requests\n");
    }

    // Contract 3: Connection: close causes connection shutdown after response
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::http::HttpServer server(&loop, mini::net::InetAddress(port, true), "contract_close");

        std::promise<void> requestDone;
        auto requestFuture = requestDone.get_future();

        server.setHttpCallback([&](const mini::http::HttpRequest& req, mini::http::HttpResponse* resp) {
            resp->setStatusCode(mini::http::HttpResponse::k200Ok);
            resp->setStatusMessage("OK");
            resp->setBody("closed");
            requestDone.set_value();
        });
        server.start();

        std::thread client([port] {
            int fd = connectTo(port);
            std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
            ::write(fd, req.data(), req.size());
            std::string resp = readAll(fd);
            assert(resp.find("Connection: close\r\n") != std::string::npos);
            assert(resp.find("closed") != std::string::npos);
            ::close(fd);
        });

        std::thread timer([&] {
            assert(requestFuture.wait_for(3s) == std::future_status::ready);
            std::this_thread::sleep_for(100ms);
            loop.queueInLoop([&loop] { loop.quit(); });
        });

        loop.loop();
        client.join();
        timer.join();
        std::printf("  PASS: Connection: close shuts down connection\n");
    }

    // Contract 4: Malformed request returns 400 Bad Request
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::http::HttpServer server(&loop, mini::net::InetAddress(port, true), "contract_400");

        server.setHttpCallback([](const mini::http::HttpRequest&, mini::http::HttpResponse* resp) {
            // Should NOT be called for malformed request.
            assert(false && "HttpCallback should not fire for malformed request");
        });
        server.start();

        std::thread client([port] {
            int fd = connectTo(port);
            std::string req = "GARBAGE REQUEST\r\n\r\n";
            ::write(fd, req.data(), req.size());
            std::string resp = readAll(fd);
            assert(resp.find("HTTP/1.1 400 Bad Request") != std::string::npos);
            assert(resp.find("Connection: close") != std::string::npos);
            ::close(fd);
        });

        std::thread timer([&] {
            std::this_thread::sleep_for(1s);
            loop.queueInLoop([&loop] { loop.quit(); });
        });

        loop.loop();
        client.join();
        timer.join();
        std::printf("  PASS: malformed request returns 400\n");
    }

    std::printf("All HttpServer contract tests passed.\n");
    return 0;
}
