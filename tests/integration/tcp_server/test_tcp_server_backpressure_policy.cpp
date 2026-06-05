#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <future>
#include <netinet/in.h>
#include <string>
#include <string_view>
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
    mini::net::TcpServer server(&loop, mini::net::InetAddress(port, true), "backpressure_policy_server");
    server.setThreadNum(1);
    server.setBackpressurePolicy(1024, 512);

    std::promise<void> firstHandled;
    std::promise<void> secondHandled;
    std::promise<void> disconnected;
    auto firstHandledFuture = firstHandled.get_future().share();
    auto secondHandledFuture = secondHandled.get_future().share();
    auto disconnectedFuture = disconnected.get_future();
    int connectionEvents = 0;

    server.setConnectionCallback([&](const mini::net::TcpConnectionPtr& connection) {
        ++connectionEvents;
        if (!connection->connected()) {
            disconnected.set_value();
            loop.quit();
        }
    });
    server.setMessageCallback([&](const mini::net::TcpConnectionPtr& connection, mini::net::Buffer* buffer) {
        const std::string message = buffer->retrieveAllAsString();
        if (message == "first") {
            connection->send(std::string(1 << 20, 'y'));
            firstHandled.set_value();
            return;
        }
        if (message == "second") {
            secondHandled.set_value();
            connection->forceClose();
        }
    });

    server.start();

    std::thread client([port, firstHandledFuture, secondHandledFuture] {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        assert(fd >= 0);

        const int receiveBufferSize = 1024;
        const int setBufferRc = ::setsockopt(
            fd,
            SOL_SOCKET,
            SO_RCVBUF,
            &receiveBufferSize,
            static_cast<socklen_t>(sizeof(receiveBufferSize)));
        assert(setBufferRc == 0);

        timeval timeout{};
        timeout.tv_sec = 2;
        const int timeoutSet =
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, static_cast<socklen_t>(sizeof(timeout)));
        assert(timeoutSet == 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        const int converted = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        assert(converted == 1);

        const int connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        assert(connected == 0);

        constexpr std::string_view firstPayload = "first";
        const ssize_t firstWritten = ::write(fd, firstPayload.data(), firstPayload.size());
        assert(firstWritten == static_cast<ssize_t>(firstPayload.size()));
        assert(firstHandledFuture.wait_for(1s) == std::future_status::ready);

        constexpr std::string_view secondPayload = "second";
        const ssize_t secondWritten = ::write(fd, secondPayload.data(), secondPayload.size());
        assert(secondWritten == static_cast<ssize_t>(secondPayload.size()));
        assert(secondHandledFuture.wait_for(150ms) == std::future_status::timeout);

        char buffer[8192];
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        std::size_t drainedBytes = 0;
        while (secondHandledFuture.wait_for(0ms) != std::future_status::ready &&
               std::chrono::steady_clock::now() < deadline) {
            const ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n > 0) {
                drainedBytes += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                std::this_thread::sleep_for(10ms);
                continue;
            }
            break;
        }

        assert(drainedBytes > 0);
        assert(secondHandledFuture.wait_for(1s) == std::future_status::ready);

        bool sawEof = false;
        const auto eofDeadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < eofDeadline) {
            const ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n > 0) {
                continue;
            }
            if (n == 0) {
                sawEof = true;
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(10ms);
                continue;
            }
            break;
        }
        assert(sawEof);

        ::close(fd);
    });

    loop.loop();
    client.join();

    assert(connectionEvents >= 2);
    assert(disconnectedFuture.wait_for(0s) == std::future_status::ready);

    return 0;
}
