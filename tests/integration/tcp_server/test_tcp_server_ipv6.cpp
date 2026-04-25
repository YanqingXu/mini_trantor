// IPv6 integration test — threaded echo server over IPv6.
//
// Verifies that the full stack works with IPv6 in a multi-threaded
// EventLoopThreadPool configuration.

#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpClient.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <cassert>
#include <chrono>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

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
    // Integration 1: Multi-threaded IPv6 echo server
    {
        mini::net::EventLoop loop;
        const uint16_t port = findFreeIpv6Port();

        mini::net::TcpServer server(&loop, mini::net::InetAddress("::1", port), "ipv6_threaded_echo");
        server.setMessageCallback([](const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
            conn->send(buf->retrieveAllAsString());
        });
        server.setThreadNum(2);
        server.start();

        const int kClients = 3;
        std::vector<std::string> received(kClients);
        std::vector<bool> connected(kClients, false);
        std::vector<std::unique_ptr<mini::net::TcpClient>> clients;

        for (int i = 0; i < kClients; ++i) {
            auto client = std::make_unique<mini::net::TcpClient>(
                &loop, mini::net::InetAddress("::1", port), "ipv6_client_" + std::to_string(i));
            int idx = i;
            client->setConnectionCallback([idx, &connected](const mini::net::TcpConnectionPtr& conn) {
                if (conn->connected()) {
                    connected[idx] = true;
                    conn->send("msg_" + std::to_string(idx));
                }
            });
            client->setMessageCallback([idx, &received](const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
                received[idx] = buf->retrieveAllAsString();
                conn->shutdown();
            });
            client->connect();
            clients.push_back(std::move(client));
        }

        loop.runAfter(3s, [&loop] { loop.quit(); });
        loop.loop();

        for (int i = 0; i < kClients; ++i) {
            assert(connected[i]);
            assert(received[i] == "msg_" + std::to_string(i));
        }
    }

    return 0;
}
