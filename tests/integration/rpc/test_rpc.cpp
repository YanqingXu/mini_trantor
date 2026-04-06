// Integration tests for RPC — full round-trip with RpcClient + RpcServer.
// Tests: callback call, coroutine call, timeout.

#include "mini/rpc/RpcClient.h"
#include "mini/rpc/RpcServer.h"
#include "mini/coroutine/Task.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"

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
using namespace mini::rpc;

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

void sendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::write(fd, data.data() + sent, data.size() - sent);
        assert(n > 0);
        sent += static_cast<std::size_t>(n);
    }
}

std::string readOneFrame(int fd) {
    char hdr[4];
    std::size_t hdrRead = 0;
    while (hdrRead < 4) {
        ssize_t n = ::read(fd, hdr + hdrRead, 4 - hdrRead);
        assert(n > 0);
        hdrRead += static_cast<std::size_t>(n);
    }
    auto* b = reinterpret_cast<const uint8_t*>(hdr);
    uint32_t bodyLen = (static_cast<uint32_t>(b[0]) << 24) |
                       (static_cast<uint32_t>(b[1]) << 16) |
                       (static_cast<uint32_t>(b[2]) << 8) |
                       static_cast<uint32_t>(b[3]);
    std::string frame(hdr, 4);
    std::string body(bodyLen, '\0');
    std::size_t bodyRead = 0;
    while (bodyRead < bodyLen) {
        ssize_t n = ::read(fd, body.data() + bodyRead, bodyLen - bodyRead);
        assert(n > 0);
        bodyRead += static_cast<std::size_t>(n);
    }
    frame.append(body);
    return frame;
}

}  // namespace

int main() {
    // Integration 1: Full RPC round-trip — server on main loop, raw socket client
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        RpcServer server(&loop, mini::net::InetAddress(port, true), "integ_rpc_srv");

        server.registerMethod("Greet",
            [](std::string_view payload,
               std::function<void(std::string_view)> respond,
               std::function<void(std::string_view)>) {
                respond(std::string("Hello, ") + std::string(payload) + "!");
            });

        std::promise<void> done;
        auto doneFuture = done.get_future();

        server.start();

        std::thread client([port, &done] {
            std::this_thread::sleep_for(50ms);
            int fd = connectTo(port);

            std::string req = codec::encodeRequest(1, "Greet", "World");
            sendAll(fd, req);

            std::string respFrame = readOneFrame(fd);
            RpcMessage msg;
            std::size_t consumed = 0;
            auto result = codec::decode(respFrame.data(), respFrame.size(), msg, consumed);
            assert(result == RpcDecodeResult::kComplete);
            assert(msg.requestId == 1);
            assert(msg.msgType == RpcMsgType::kResponse);
            assert(msg.payload == "Hello, World!");

            ::close(fd);
            done.set_value();
        });

        std::thread timer([&] {
            assert(doneFuture.wait_for(3s) == std::future_status::ready);
            std::this_thread::sleep_for(50ms);
            loop.queueInLoop([&loop] { loop.quit(); });
        });

        loop.loop();
        client.join();
        timer.join();
        std::printf("  PASS: full RPC round-trip (raw socket)\n");
    }

    // Integration 2: RpcClient callback call — both server and client on same loop
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;

        RpcServer server(&loop, mini::net::InetAddress(port, true), "cb_rpc_srv");
        server.registerMethod("Echo",
            [](std::string_view payload,
               std::function<void(std::string_view)> respond,
               std::function<void(std::string_view)>) {
                respond(payload);
            });
        server.start();

        RpcClient client(&loop, mini::net::InetAddress(port, true), "cb_rpc_cli");

        bool gotResult = false;
        std::string resultPayload;

        client.setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (conn->connected()) {
                client.call("Echo", "ping",
                    [&](const std::string& error, const std::string& payload) {
                        assert(error.empty());
                        gotResult = true;
                        resultPayload = payload;
                        loop.queueInLoop([&loop] { loop.quit(); });
                    }, 3000);
            }
        });

        auto timerId = loop.runAfter(3s, [&loop] { loop.quit(); });

        client.connect();
        loop.loop();

        assert(gotResult);
        assert(resultPayload == "ping");
        std::printf("  PASS: RpcClient callback call\n");
    }

    // Integration 3: Coroutine RPC call
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;

        RpcServer server(&loop, mini::net::InetAddress(port, true), "coro_rpc_srv");
        server.registerMethod("Double",
            [](std::string_view payload,
               std::function<void(std::string_view)> respond,
               std::function<void(std::string_view)>) {
                respond(std::string(payload) + std::string(payload));
            });
        server.start();

        RpcClient client(&loop, mini::net::InetAddress(port, true), "coro_rpc_cli");

        bool gotResult = false;
        std::string resultPayload;

        auto coroHandler = [&]() -> mini::coroutine::Task<void> {
            auto result = co_await client.asyncCall("Double", "abc", 3000);
            assert(result.ok());
            gotResult = true;
            resultPayload = result.payload;
            loop.queueInLoop([&loop] { loop.quit(); });
        };

        client.setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (conn->connected()) {
                auto task = coroHandler();
                task.detach();
            }
        });

        auto timerId = loop.runAfter(3s, [&loop] { loop.quit(); });

        client.connect();
        loop.loop();

        assert(gotResult);
        assert(resultPayload == "abcabc");
        std::printf("  PASS: coroutine RPC call\n");
    }

    // Integration 4: RPC timeout
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;

        RpcServer server(&loop, mini::net::InetAddress(port, true), "timeout_rpc_srv");
        server.registerMethod("Slow",
            [](std::string_view,
               std::function<void(std::string_view)>,
               std::function<void(std::string_view)>) {
                // Never respond — force timeout
            });
        server.start();

        RpcClient client(&loop, mini::net::InetAddress(port, true), "timeout_rpc_cli");

        bool gotTimeout = false;

        client.setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (conn->connected()) {
                client.call("Slow", "data",
                    [&](const std::string& error, const std::string&) {
                        gotTimeout = !error.empty();
                        assert(error.find("timed out") != std::string::npos);
                        loop.queueInLoop([&loop] { loop.quit(); });
                    }, 200);  // 200ms timeout
            }
        });

        auto timerId = loop.runAfter(3s, [&loop] { loop.quit(); });

        client.connect();
        loop.loop();

        assert(gotTimeout);
        std::printf("  PASS: RPC timeout\n");
    }

    std::printf("All RPC integration tests passed.\n");
    return 0;
}
