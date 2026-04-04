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

using namespace std::chrono_literals;

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
    mini::net::TcpServer server(&loop, mini::net::InetAddress(port, true), "backpressure_server");
    server.setThreadNum(1);

    std::promise<std::pair<std::thread::id, std::size_t>> highWater;
    auto highWaterFuture = highWater.get_future();
    std::promise<void> disconnected;
    auto disconnectedFuture = disconnected.get_future();
    int connectionEvents = 0;

    server.setHighWaterMarkCallback([&](const mini::net::TcpConnectionPtr& connection, std::size_t bytes) {
        assert(connection->getLoop()->isInLoopThread());
        highWater.set_value({std::this_thread::get_id(), bytes});
        connection->forceClose();
    }, 1024);
    server.setConnectionCallback([&](const mini::net::TcpConnectionPtr& connection) {
        ++connectionEvents;
        if (connection->connected()) {
            std::string payload(1 << 20, 'y');
            connection->send(payload);
            return;
        }
        disconnected.set_value();
        loop.quit();
    });

    server.start();

    std::thread client([port] {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        assert(fd >= 0);

        const int receiveBufferSize = 1024;
        const int setRc = ::setsockopt(
            fd,
            SOL_SOCKET,
            SO_RCVBUF,
            &receiveBufferSize,
            static_cast<socklen_t>(sizeof(receiveBufferSize)));
        assert(setRc == 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        const int converted = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        assert(converted == 1);

        const int connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        assert(connected == 0);

        std::this_thread::sleep_for(300ms);
        ::close(fd);
    });

    loop.loop();
    client.join();

    assert(highWaterFuture.wait_for(0s) == std::future_status::ready);
    const auto [firedThread, bytes] = highWaterFuture.get();
    assert(firedThread != std::this_thread::get_id());
    assert(bytes >= 1024);
    assert(disconnectedFuture.wait_for(0s) == std::future_status::ready);
    assert(connectionEvents >= 2);

    return 0;
}
