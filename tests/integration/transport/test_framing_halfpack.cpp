// Integration test for PacketFramer + RPC path with half-pack (分片粘包) payload.
// Validate: request frame split writes still decode as one full RPC frame and return valid response.

#include "mini/rpc/RpcCodec.h"
#include "mini/rpc/RpcServer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/framing/PacketFramer.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <future>
#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using namespace mini::rpc;
using namespace mini::net;
using namespace mini::net::framing;

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

std::string readExactly(int fd, std::size_t expected) {
    std::string result;
    result.reserve(expected);
    char buf[4096];
    while (result.size() < expected) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        assert(n > 0);
        result.append(buf, static_cast<std::size_t>(n));
    }
    return result;
}

std::string readOneFrame(int fd) {
    std::string header;
    header.resize(kFrameHeaderSize);
    std::size_t headerRead = 0;
    while (headerRead < kFrameHeaderSize) {
        ssize_t n = ::read(fd, header.data() + headerRead,
                           static_cast<int>(kFrameHeaderSize - headerRead));
        assert(n > 0);
        headerRead += static_cast<std::size_t>(n);
    }

    const auto* b = reinterpret_cast<const std::uint8_t*>(header.data());
    const std::uint32_t payloadLen = (static_cast<uint32_t>(b[2]) << 24) |
                                     (static_cast<uint32_t>(b[3]) << 16) |
                                     (static_cast<uint32_t>(b[4]) << 8) |
                                     static_cast<uint32_t>(b[5]);
    std::string frame = std::move(header);
    frame.append(readExactly(fd, payloadLen));
    return frame;
}

}  // namespace

int main() {
    const uint16_t port = allocateTestPort();
    mini::net::EventLoop loop;
    RpcServer server(&loop, mini::net::InetAddress(port, true), "framing_halfpack");

    server.registerMethod("Echo",
                         [](std::string_view payload,
                            std::function<void(std::string_view)> respond,
                            std::function<void(std::string_view)>) {
                             respond(std::string("pong:") + std::string(payload));
                         });
    server.start();

    std::promise<void> done;
    auto doneFuture = done.get_future();

    std::thread client([port, &done] {
        std::this_thread::sleep_for(30ms);
        int fd = connectTo(port);
        std::string req = codec::encodeRequest(123, "Echo", "split-payload");

        const std::size_t firstChunk = req.size() / 2;
        sendAll(fd, req.substr(0, firstChunk));
        std::this_thread::sleep_for(5ms);
        sendAll(fd, req.substr(firstChunk));

        std::string respFrame = readOneFrame(fd);
        RpcMessage msg;
        std::size_t consumed = 0;
        auto state = codec::decode(respFrame.data(), respFrame.size(), msg, consumed);
        assert(state == RpcDecodeResult::kComplete);
        assert(msg.requestId == 123);
        assert(msg.msgType == RpcMsgType::kResponse);
        assert(msg.payload == "pong:split-payload");
        ::close(fd);
        done.set_value();
    });

    std::thread timer([&] {
        assert(doneFuture.wait_for(3s) == std::future_status::ready);
        std::this_thread::sleep_for(30ms);
        loop.queueInLoop([&loop] { loop.quit(); });
    });

    loop.loop();
    client.join();
    timer.join();

    std::printf("  PASS: half-packet RPC transport still decodes via PacketFramer\n");
    return 0;
}
