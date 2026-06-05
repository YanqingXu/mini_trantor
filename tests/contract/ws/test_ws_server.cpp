// Contract tests for WebSocketServer.
// Verifies: upgrade handshake, message delivery, ping/pong, close handshake,
// invalid upgrade rejection.

#include "mini/ws/WebSocketCodec.h"
#include "mini/ws/WebSocketServer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdint>
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

std::string makeUpgradeRequest(const std::string& key = "dGhlIHNhbXBsZSBub25jZQ==") {
    return "GET /ws HTTP/1.1\r\n"
           "Host: localhost\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Key: " + key + "\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "\r\n";
}

// Build a masked client frame for testing
std::string buildMaskedFrame(mini::ws::WsOpcode opcode, const std::string& payload,
                             const std::uint8_t mask[4] = nullptr) {
    std::uint8_t defaultMask[4] = {0x37, 0xFA, 0x21, 0x3D};
    const std::uint8_t* maskKey = mask ? mask : defaultMask;

    std::string frame;
    frame.push_back(static_cast<char>(0x80 | static_cast<std::uint8_t>(opcode)));

    // Payload length with mask bit set
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((payload.size() >> (i * 8)) & 0xFF));
        }
    }

    // Mask key
    frame.append(reinterpret_cast<const char*>(maskKey), 4);

    // Masked payload
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ maskKey[i % 4]));
    }
    return frame;
}

std::string buildMaskedCloseFrame(mini::ws::WsCloseCode code, const std::string& reason = "") {
    std::string payload;
    auto codeVal = static_cast<std::uint16_t>(code);
    payload.push_back(static_cast<char>((codeVal >> 8) & 0xFF));
    payload.push_back(static_cast<char>(codeVal & 0xFF));
    payload.append(reason);
    return buildMaskedFrame(mini::ws::WsOpcode::kClose, payload);
}

}  // namespace

int main() {
    // Contract 1: Upgrade handshake produces 101 Switching Protocols
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::ws::WebSocketServer server(&loop, mini::net::InetAddress(port, true), "ws_upgrade");

        std::promise<bool> connected;
        auto connFuture = connected.get_future();

        server.setConnectCallback([&](const mini::net::TcpConnectionPtr&) {
            connected.set_value(true);
        });
        server.setMessageCallback([](const mini::net::TcpConnectionPtr&,
                                     std::string, mini::ws::WsOpcode) {});
        server.start();

        std::thread client([port] {
            int fd = connectTo(port);
            sendAll(fd, makeUpgradeRequest());

            // Read 101 response
            std::string resp;
            char buf[4096];
            for (;;) {
                ssize_t n = ::read(fd, buf, sizeof(buf));
                if (n <= 0) break;
                resp.append(buf, static_cast<std::size_t>(n));
                if (resp.find("\r\n\r\n") != std::string::npos) break;
            }

            assert(resp.find("HTTP/1.1 101 Switching Protocols") != std::string::npos);
            assert(resp.find("Upgrade: websocket") != std::string::npos);
            assert(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
            ::close(fd);
        });

        std::thread timer([&] {
            assert(connFuture.wait_for(3s) == std::future_status::ready);
            assert(connFuture.get());
            std::this_thread::sleep_for(100ms);
            loop.queueInLoop([&loop] { loop.quit(); });
        });

        loop.loop();
        client.join();
        timer.join();
        std::printf("  PASS: upgrade handshake produces 101\n");
    }

    // Contract 2: Text message echo
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::ws::WebSocketServer server(&loop, mini::net::InetAddress(port, true), "ws_echo");

        std::promise<void> done;
        auto doneFuture = done.get_future();

        server.setMessageCallback([](const mini::net::TcpConnectionPtr& conn,
                                     std::string msg, mini::ws::WsOpcode) {
            mini::ws::WebSocketConnection::sendText(conn, msg);
        });
        server.start();

        std::thread client([port, &done] {
            int fd = connectTo(port);
            sendAll(fd, makeUpgradeRequest());

            // Read 101
            std::string resp;
            char buf[4096];
            for (;;) {
                ssize_t n = ::read(fd, buf, sizeof(buf));
                if (n <= 0) break;
                resp.append(buf, static_cast<std::size_t>(n));
                if (resp.find("\r\n\r\n") != std::string::npos) break;
            }

            // Send masked text frame "hello ws"
            std::string frame = buildMaskedFrame(mini::ws::WsOpcode::kText, "hello ws");
            sendAll(fd, frame);

            // Read echo response — server sends unmasked frame
            // Expected: 2 header + 8 payload = 10 bytes
            std::string echoFrame = readN(fd, 10);
            assert(echoFrame.size() == 10);
            auto* b = reinterpret_cast<const std::uint8_t*>(echoFrame.data());
            assert(b[0] == 0x81);  // FIN=1, text
            assert(b[1] == 8);     // no mask, len=8
            assert(echoFrame.substr(2) == "hello ws");

            done.set_value();
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
        std::printf("  PASS: text message echo\n");
    }

    // Contract 3: Ping gets automatic pong
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::ws::WebSocketServer server(&loop, mini::net::InetAddress(port, true), "ws_ping");

        server.setMessageCallback([](const mini::net::TcpConnectionPtr&,
                                     std::string, mini::ws::WsOpcode) {});
        server.start();

        std::promise<void> done;
        auto doneFuture = done.get_future();

        std::thread client([port, &done] {
            int fd = connectTo(port);
            sendAll(fd, makeUpgradeRequest());

            // Read 101
            std::string resp;
            char buf[4096];
            for (;;) {
                ssize_t n = ::read(fd, buf, sizeof(buf));
                if (n <= 0) break;
                resp.append(buf, static_cast<std::size_t>(n));
                if (resp.find("\r\n\r\n") != std::string::npos) break;
            }

            // Send masked ping "test"
            std::string pingFrame = buildMaskedFrame(mini::ws::WsOpcode::kPing, "test");
            sendAll(fd, pingFrame);

            // Read pong response: 2 header + 4 payload = 6 bytes
            std::string pongFrame = readN(fd, 6);
            assert(pongFrame.size() == 6);
            auto* b = reinterpret_cast<const std::uint8_t*>(pongFrame.data());
            assert(b[0] == 0x8A);  // FIN=1, pong
            assert(b[1] == 4);
            assert(pongFrame.substr(2) == "test");

            done.set_value();
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
        std::printf("  PASS: ping gets automatic pong\n");
    }

    // Contract 4: Close handshake
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::ws::WebSocketServer server(&loop, mini::net::InetAddress(port, true), "ws_close");

        std::promise<mini::ws::WsCloseCode> closedCode;
        auto closeFuture = closedCode.get_future();

        server.setMessageCallback([](const mini::net::TcpConnectionPtr&,
                                     std::string, mini::ws::WsOpcode) {});
        server.setCloseCallback([&](const mini::net::TcpConnectionPtr&,
                                    mini::ws::WsCloseCode code, const std::string&) {
            closedCode.set_value(code);
        });
        server.start();

        std::thread client([port] {
            int fd = connectTo(port);
            sendAll(fd, makeUpgradeRequest());

            // Read 101
            std::string resp;
            char buf[4096];
            for (;;) {
                ssize_t n = ::read(fd, buf, sizeof(buf));
                if (n <= 0) break;
                resp.append(buf, static_cast<std::size_t>(n));
                if (resp.find("\r\n\r\n") != std::string::npos) break;
            }

            // Send masked close frame
            std::string closeFrame = buildMaskedCloseFrame(mini::ws::WsCloseCode::kNormal, "bye");
            sendAll(fd, closeFrame);

            // Read close echo — server sends back close frame
            std::string serverClose = readAll(fd);
            assert(!serverClose.empty());
            // Verify it's a close frame
            auto* b = reinterpret_cast<const std::uint8_t*>(serverClose.data());
            assert((b[0] & 0x0F) == 0x08);  // opcode = close

            ::close(fd);
        });

        std::thread timer([&] {
            assert(closeFuture.wait_for(3s) == std::future_status::ready);
            assert(closeFuture.get() == mini::ws::WsCloseCode::kNormal);
            std::this_thread::sleep_for(100ms);
            loop.queueInLoop([&loop] { loop.quit(); });
        });

        loop.loop();
        client.join();
        timer.join();
        std::printf("  PASS: close handshake\n");
    }

    // Contract 5: Invalid upgrade request gets 400
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::ws::WebSocketServer server(&loop, mini::net::InetAddress(port, true), "ws_bad");

        server.setMessageCallback([](const mini::net::TcpConnectionPtr&,
                                     std::string, mini::ws::WsOpcode) {});
        server.start();

        std::promise<void> done;
        auto doneFuture = done.get_future();

        std::thread client([port, &done] {
            int fd = connectTo(port);
            // Send a plain HTTP GET (not an upgrade)
            std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
            sendAll(fd, req);
            std::string resp = readAll(fd);
            assert(resp.find("400 Bad Request") != std::string::npos);
            done.set_value();
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
        std::printf("  PASS: invalid upgrade gets 400\n");
    }

    std::printf("All WebSocketServer contract tests passed.\n");
    return 0;
}
