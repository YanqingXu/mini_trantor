// Contract tests for RpcServer — method registration, dispatch, error handling.
// Tests: callback handler, coroutine handler, error handling, sequential requests.

#include "mini/rpc/RpcCodec.h"
#include "mini/rpc/RpcServer.h"
#include "mini/coroutine/Task.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
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

std::string readExactly(int fd, std::size_t expected, int timeoutSec = 3) {
    std::string result;
    result.reserve(expected);
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    while (result.size() < expected) {
        if (std::chrono::steady_clock::now() > deadline) {
            break;
        }
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            result.append(buf, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            break;
        }
    }
    return result;
}

// Read one full RPC frame from fd
std::string readOneFrame(int fd) {
    // First read 4-byte header
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
    std::string body = readExactly(fd, bodyLen);
    assert(body.size() == bodyLen);
    frame.append(body);
    return frame;
}

}  // namespace

int main() {
    // 1. RPC request dispatched to registered handler, response returned
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        RpcServer server(&loop, mini::net::InetAddress(port, true), "rpc_contract");

        server.registerMethod("Echo",
            [](std::string_view payload,
               std::function<void(std::string_view)> respond,
               std::function<void(std::string_view)>) {
                respond(std::string("echo:") + std::string(payload));
            });

        std::promise<void> done;
        auto doneFuture = done.get_future();

        server.start();

        std::thread client([port, &done] {
            std::this_thread::sleep_for(50ms);
            int fd = connectTo(port);

            // Send an RPC request
            std::string req = codec::encodeRequest(1, "Echo", "hello");
            sendAll(fd, req);

            // Read the response
            std::string respFrame = readOneFrame(fd);
            RpcMessage msg;
            std::size_t consumed = 0;
            auto result = codec::decode(respFrame.data(), respFrame.size(), msg, consumed);
            assert(result == RpcDecodeResult::kComplete);
            assert(msg.requestId == 1);
            assert(msg.msgType == RpcMsgType::kResponse);
            assert(msg.payload == "echo:hello");

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
        std::printf("  PASS: request dispatched to handler, response returned\n");
    }

    // 2. Unregistered method returns error response
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        RpcServer server(&loop, mini::net::InetAddress(port, true), "rpc_unregistered");

        std::promise<void> done;
        auto doneFuture = done.get_future();

        server.start();

        std::thread client([port, &done] {
            std::this_thread::sleep_for(50ms);
            int fd = connectTo(port);

            std::string req = codec::encodeRequest(42, "NoSuchMethod", "data");
            sendAll(fd, req);

            std::string respFrame = readOneFrame(fd);
            RpcMessage msg;
            std::size_t consumed = 0;
            auto result = codec::decode(respFrame.data(), respFrame.size(), msg, consumed);
            assert(result == RpcDecodeResult::kComplete);
            assert(msg.requestId == 42);
            assert(msg.msgType == RpcMsgType::kError);
            assert(msg.payload.find("not found") != std::string::npos);

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
        std::printf("  PASS: unregistered method returns error\n");
    }

    // 3. Multiple sequential requests on same connection
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        RpcServer server(&loop, mini::net::InetAddress(port, true), "rpc_sequential");

        server.registerMethod("Add",
            [](std::string_view payload,
               std::function<void(std::string_view)> respond,
               std::function<void(std::string_view)>) {
                respond(std::string(payload) + "+1");
            });

        std::promise<void> done;
        auto doneFuture = done.get_future();

        server.start();

        std::thread client([port, &done] {
            std::this_thread::sleep_for(50ms);
            int fd = connectTo(port);

            for (int i = 1; i <= 3; ++i) {
                std::string payload = "val" + std::to_string(i);
                std::string req = codec::encodeRequest(
                    static_cast<uint64_t>(i), "Add", payload);
                sendAll(fd, req);

                std::string respFrame = readOneFrame(fd);
                RpcMessage msg;
                std::size_t consumed = 0;
                auto result = codec::decode(respFrame.data(), respFrame.size(),
                                            msg, consumed);
                assert(result == RpcDecodeResult::kComplete);
                assert(msg.requestId == static_cast<uint64_t>(i));
                assert(msg.payload == payload + "+1");
            }

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
        std::printf("  PASS: multiple sequential requests\n");
    }

    // 4. Error response from handler
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        RpcServer server(&loop, mini::net::InetAddress(port, true), "rpc_handler_error");

        server.registerMethod("Fail",
            [](std::string_view,
               std::function<void(std::string_view)>,
               std::function<void(std::string_view)> respondError) {
                respondError("intentional error");
            });

        std::promise<void> done;
        auto doneFuture = done.get_future();

        server.start();

        std::thread client([port, &done] {
            std::this_thread::sleep_for(50ms);
            int fd = connectTo(port);

            std::string req = codec::encodeRequest(1, "Fail", "");
            sendAll(fd, req);

            std::string respFrame = readOneFrame(fd);
            RpcMessage msg;
            std::size_t consumed = 0;
            auto result = codec::decode(respFrame.data(), respFrame.size(),
                                        msg, consumed);
            assert(result == RpcDecodeResult::kComplete);
            assert(msg.msgType == RpcMsgType::kError);
            assert(msg.payload == "intentional error");

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
        std::printf("  PASS: handler error response\n");
    }

    // 5. Coroutine handler: registerCoroMethod dispatches and sends response
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        RpcServer server(&loop, mini::net::InetAddress(port, true), "rpc_coro_handler");

        server.registerCoroMethod("CoroEcho",
            [](std::string payload) -> mini::coroutine::Task<std::string> {
                co_return "coro:" + payload;
            });

        std::promise<void> done;
        auto doneFuture = done.get_future();

        server.start();

        std::thread client([port, &done] {
            std::this_thread::sleep_for(50ms);
            int fd = connectTo(port);

            std::string req = codec::encodeRequest(1, "CoroEcho", "world");
            sendAll(fd, req);

            std::string respFrame = readOneFrame(fd);
            RpcMessage msg;
            std::size_t consumed = 0;
            auto result = codec::decode(respFrame.data(), respFrame.size(), msg, consumed);
            assert(result == RpcDecodeResult::kComplete);
            assert(msg.requestId == 1);
            assert(msg.msgType == RpcMsgType::kResponse);
            assert(msg.payload == "coro:world");

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
        std::printf("  PASS: coroutine handler dispatched, response returned\n");
    }

    // 6. Coroutine handler throws exception → error response
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        RpcServer server(&loop, mini::net::InetAddress(port, true), "rpc_coro_error");

        server.registerCoroMethod("CoroFail",
            [](std::string) -> mini::coroutine::Task<std::string> {
                throw std::runtime_error("coro handler error");
                co_return "";  // unreachable, needed for coroutine
            });

        std::promise<void> done;
        auto doneFuture = done.get_future();

        server.start();

        std::thread client([port, &done] {
            std::this_thread::sleep_for(50ms);
            int fd = connectTo(port);

            std::string req = codec::encodeRequest(1, "CoroFail", "");
            sendAll(fd, req);

            std::string respFrame = readOneFrame(fd);
            RpcMessage msg;
            std::size_t consumed = 0;
            auto result = codec::decode(respFrame.data(), respFrame.size(), msg, consumed);
            assert(result == RpcDecodeResult::kComplete);
            assert(msg.requestId == 1);
            assert(msg.msgType == RpcMsgType::kError);
            assert(msg.payload == "coro handler error");

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
        std::printf("  PASS: coroutine handler exception → error response\n");
    }

    std::printf("All RpcServer contract tests passed.\n");
    return 0;
}
