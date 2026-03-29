#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <future>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

uint16_t allocateTestPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    const int bound = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    assert(bound == 0);

    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    const int named = ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    assert(named == 0);

    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

}  // namespace

int main() {
    const uint16_t port = allocateTestPort();

    mini::net::EventLoop loop;
    mini::net::TcpServer server(&loop, mini::net::InetAddress(port, true), "test_echo_server");

    int connectionEvents = 0;
    int messageEvents = 0;
    std::promise<void> disconnected;
    auto disconnectedFuture = disconnected.get_future();

    server.setConnectionCallback([&](const mini::net::TcpConnectionPtr& connection) {
        ++connectionEvents;
        if (!connection->connected()) {
            disconnected.set_value();
            loop.quit();
        }
    });
    server.setMessageCallback([&](const mini::net::TcpConnectionPtr& connection, mini::net::Buffer* buffer) {
        ++messageEvents;
        connection->send(buffer->retrieveAllAsString());
    });

    server.start();

    std::thread client([port] {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        assert(fd >= 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        const int converted = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        assert(converted == 1);

        const int connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        assert(connected == 0);

        const std::string payload = "hello reactor";
        const ssize_t written = ::write(fd, payload.data(), payload.size());
        assert(written == static_cast<ssize_t>(payload.size()));

        char buffer[64] = {};
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        assert(n == static_cast<ssize_t>(payload.size()));
        assert(std::string(buffer, static_cast<std::size_t>(n)) == payload);

        ::close(fd);
    });

    loop.loop();
    client.join();

    assert(messageEvents == 1);
    assert(connectionEvents >= 2);
    assert(disconnectedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);

    return 0;
}
