// Integration test for WebSocket: full echo server round-trip with
// multiple messages, binary data, and multi-threaded server.

#include "mini/ws/WebSocketCodec.h"
#include "mini/ws/WebSocketConnection.h"
#include "mini/ws/WebSocketServer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdint>
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

void sendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::write(fd, data.data() + sent, data.size() - sent);
        assert(n > 0);
        sent += static_cast<std::size_t>(n);
    }
}

std::string readN(int fd, std::size_t n) {
    std::string result;
    result.resize(n);
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, result.data() + got, n - got);
        if (r <= 0) break;
        got += static_cast<std::size_t>(r);
    }
    result.resize(got);
    return result;
}

std::string makeUpgradeRequest() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: localhost\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "\r\n";
}

std::string buildMaskedFrame(mini::ws::WsOpcode opcode, const std::string& payload) {
    std::uint8_t mask[4] = {0x37, 0xFA, 0x21, 0x3D};
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | static_cast<std::uint8_t>(opcode)));
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    }
    frame.append(reinterpret_cast<const char*>(mask), 4);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    }
    return frame;
}

// Do the upgrade handshake, return fd. Reads until \r\n\r\n found.
int upgradeConnection(uint16_t port) {
    int fd = connectTo(port);
    sendAll(fd, makeUpgradeRequest());
    // Read 101 response
    std::string resp;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        assert(n > 0);
        resp.append(buf, static_cast<std::size_t>(n));
        if (resp.find("\r\n\r\n") != std::string::npos) break;
    }
    assert(resp.find("101 Switching Protocols") != std::string::npos);
    return fd;
}

}  // namespace

int main() {
    // Integration 1: Multiple messages on a single WebSocket connection
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::ws::WebSocketServer server(&loop, mini::net::InetAddress(port, true), "integ_ws_multi");

        std::atomic<int> msgCount{0};
        std::promise<void> allDone;
        auto allFuture = allDone.get_future();

        server.setMessageCallback([&](const mini::net::TcpConnectionPtr& conn,
                                      std::string msg, mini::ws::WsOpcode) {
            mini::ws::WebSocketConnection::sendText(conn, "reply:" + msg);
            if (++msgCount == 3) {
                allDone.set_value();
            }
        });
        server.start();

        std::thread client([port] {
            int fd = upgradeConnection(port);

            for (int i = 0; i < 3; ++i) {
                std::string payload = "msg" + std::to_string(i);
                sendAll(fd, buildMaskedFrame(mini::ws::WsOpcode::kText, payload));

                // Read echo response
                std::string expected = "reply:" + payload;
                std::size_t expectedSize = 2 + expected.size();  // header + payload
                std::string resp = readN(fd, expectedSize);
                assert(resp.size() == expectedSize);
                assert(resp.substr(2) == expected);
            }

            ::close(fd);
        });

        std::thread timer([&] {
            assert(allFuture.wait_for(3s) == std::future_status::ready);
            std::this_thread::sleep_for(100ms);
            loop.queueInLoop([&loop] { loop.quit(); });
        });

        loop.loop();
        client.join();
        timer.join();
        assert(msgCount.load() == 3);
        std::printf("  PASS: multiple messages on single connection\n");
    }

    // Integration 2: Binary message round-trip
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::ws::WebSocketServer server(&loop, mini::net::InetAddress(port, true), "integ_ws_bin");

        std::promise<void> done;
        auto doneFuture = done.get_future();

        server.setMessageCallback([&](const mini::net::TcpConnectionPtr& conn,
                                      std::string msg, mini::ws::WsOpcode opcode) {
            assert(opcode == mini::ws::WsOpcode::kBinary);
            mini::ws::WebSocketConnection::sendBinary(conn, msg);
            done.set_value();
        });
        server.start();

        std::thread client([port] {
            int fd = upgradeConnection(port);

            // Send binary data
            std::string binData = "\x00\x01\x02\xFF\xFE";
            sendAll(fd, buildMaskedFrame(mini::ws::WsOpcode::kBinary, binData));

            // Read echo
            std::size_t expectedSize = 2 + binData.size();
            std::string resp = readN(fd, expectedSize);
            assert(resp.size() == expectedSize);
            auto* b = reinterpret_cast<const std::uint8_t*>(resp.data());
            assert(b[0] == 0x82);  // FIN=1, binary
            assert(resp.substr(2) == binData);

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
        std::printf("  PASS: binary message round-trip\n");
    }

    // Integration 3: Multiple concurrent WebSocket clients
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::ws::WebSocketServer server(&loop, mini::net::InetAddress(port, true), "integ_ws_concurrent");

        std::atomic<int> totalMsgs{0};
        std::promise<void> allDone;
        auto allFuture = allDone.get_future();

        server.setMessageCallback([&](const mini::net::TcpConnectionPtr& conn,
                                      std::string msg, mini::ws::WsOpcode) {
            mini::ws::WebSocketConnection::sendText(conn, msg);
            if (++totalMsgs == 4) {
                allDone.set_value();
            }
        });
        server.start();

        auto clientFn = [port](int clientId) {
            int fd = upgradeConnection(port);

            for (int i = 0; i < 2; ++i) {
                std::string payload = "c" + std::to_string(clientId) + "m" + std::to_string(i);
                sendAll(fd, buildMaskedFrame(mini::ws::WsOpcode::kText, payload));

                std::size_t expectedSize = 2 + payload.size();
                std::string resp = readN(fd, expectedSize);
                assert(resp.size() == expectedSize);
                assert(resp.substr(2) == payload);
            }

            ::close(fd);
        };

        std::thread c1(clientFn, 0);
        std::thread c2(clientFn, 1);

        std::thread timer([&] {
            assert(allFuture.wait_for(5s) == std::future_status::ready);
            std::this_thread::sleep_for(200ms);
            loop.queueInLoop([&loop] { loop.quit(); });
        });

        loop.loop();
        c1.join();
        c2.join();
        timer.join();
        assert(totalMsgs.load() == 4);
        std::printf("  PASS: multiple concurrent clients\n");
    }

    std::printf("All WebSocket integration tests passed.\n");
    return 0;
}
