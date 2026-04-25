// TcpClient/TcpServer IPv6 contract tests.
//
// Verifies that IPv6 listen/connect/echo works through the full
// TcpServer → Acceptor → TcpConnection → TcpClient → Connector path.
//
// Core Module Change Gate:
// 1. Which loop/thread owns this module? — owner EventLoop thread.
// 2. Who owns it and who releases it? — TcpServer owns acceptor and connections.
// 3. Which callbacks may re-enter? — connection/message/close callbacks.
// 4. Cross-thread? — connect() marshals via runInLoop.
// 5. Test file? — This file.

#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpClient.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <cassert>
#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

/// Find a free port by binding to IPv6 loopback on port 0.
uint16_t findFreeIpv6Port() {
    const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_loopback;
    addr.sin6_port = htons(0);
    socklen_t len = sizeof(addr);
    assert(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const uint16_t port = ntohs(addr.sin6_port);
    ::close(fd);
    return port;
}

}  // namespace

int main() {
    // Contract 1: IPv6 echo server — basic connect + message round-trip
    {
        mini::net::EventLoop loop;
        const uint16_t port = findFreeIpv6Port();
        const mini::net::InetAddress listenAddr("::1", port);

        mini::net::TcpServer server(&loop, listenAddr, "ipv6_echo", true);
        server.setMessageCallback([](const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
            std::string msg = buf->retrieveAllAsString();
            conn->send(msg);
        });

        std::string received;
        bool connected = false;
        bool disconnected = false;

        mini::net::TcpClient client(&loop, mini::net::InetAddress("::1", port), "ipv6_client");
        client.setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (conn->connected()) {
                connected = true;
                conn->send("hello ipv6");
            } else {
                disconnected = true;
            }
        });
        client.setMessageCallback([&](const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
            received = buf->retrieveAllAsString();
            conn->shutdown();
        });

        server.start();
        client.connect();

        loop.runAfter(2s, [&loop] { loop.quit(); });
        loop.loop();

        assert(connected);
        assert(received == "hello ipv6");
    }

    // Contract 2: IPv6 peer/local address accessible as IPv6
    {
        mini::net::EventLoop loop;
        const uint16_t port = findFreeIpv6Port();
        const mini::net::InetAddress listenAddr("::1", port);

        mini::net::TcpServer server(&loop, listenAddr, "ipv6_addr", true);

        bool serverSawIpv6 = false;
        server.setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (conn->connected()) {
                serverSawIpv6 = conn->peerAddress().isIpv6();
            }
        });
        server.setMessageCallback([](const mini::net::TcpConnectionPtr&, mini::net::Buffer*) {});

        bool clientSawIpv6 = false;
        mini::net::TcpClient client(&loop, mini::net::InetAddress("::1", port), "ipv6_client2");
        client.setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (conn->connected()) {
                clientSawIpv6 = conn->peerAddress().isIpv6();
                conn->shutdown();
            }
        });
        client.setMessageCallback([](const mini::net::TcpConnectionPtr&, mini::net::Buffer*) {});

        server.start();
        client.connect();

        loop.runAfter(2s, [&loop] { loop.quit(); });
        loop.loop();

        assert(serverSawIpv6);
        assert(clientSawIpv6);
    }

    // Contract 3: IPv6 address stringification — toIpPort uses bracket notation
    {
        mini::net::EventLoop loop;
        const uint16_t port = findFreeIpv6Port();
        const mini::net::InetAddress listenAddr("::1", port);

        mini::net::TcpServer server(&loop, listenAddr, "ipv6_str", true);

        std::string peerIpPort;
        server.setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (conn->connected()) {
                peerIpPort = conn->peerAddress().toIpPort();
            }
        });
        server.setMessageCallback([](const mini::net::TcpConnectionPtr&, mini::net::Buffer*) {});

        mini::net::TcpClient client(&loop, mini::net::InetAddress("::1", port), "ipv6_client3");
        client.setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (conn->connected()) {
                conn->shutdown();
            }
        });
        client.setMessageCallback([](const mini::net::TcpConnectionPtr&, mini::net::Buffer*) {});

        server.start();
        client.connect();

        loop.runAfter(2s, [&loop] { loop.quit(); });
        loop.loop();

        // Should be in form [::1]:<port>
        assert(peerIpPort.find("[::1]:") == 0);
    }

    return 0;
}
