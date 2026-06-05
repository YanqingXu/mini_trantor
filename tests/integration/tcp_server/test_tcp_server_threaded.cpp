#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

struct WorkerInfo {
    mini::net::EventLoop* loop;
    std::thread::id threadId;
};

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
    using namespace std::chrono_literals;

    const uint16_t port = allocateTestPort();

    mini::net::EventLoop baseLoop;
    mini::net::TcpServer server(&baseLoop, mini::net::InetAddress(port, true), "threaded_echo_server");
    server.setThreadNum(1);

    std::promise<WorkerInfo> workerReady;
    auto workerFuture = workerReady.get_future();

    mini::net::EventLoop* workerLoop = nullptr;
    std::thread::id workerThreadId;
    const auto baseThreadId = std::this_thread::get_id();

    std::atomic<int> connectionEvents{0};
    std::atomic<int> messageEvents{0};
    std::promise<void> disconnected;
    auto disconnectedFuture = disconnected.get_future();

    server.setThreadInitCallback([&](mini::net::EventLoop* loop) {
        workerReady.set_value(WorkerInfo{loop, std::this_thread::get_id()});
    });

    server.setConnectionCallback([&](const mini::net::TcpConnectionPtr& connection) {
        ++connectionEvents;
        assert(connection->getLoop() == workerLoop);
        assert(connection->getLoop() != &baseLoop);
        assert(connection->getLoop()->isInLoopThread());
        assert(std::this_thread::get_id() == workerThreadId);
        assert(std::this_thread::get_id() != baseThreadId);

        if (!connection->connected()) {
            disconnected.set_value();
            baseLoop.quit();
        }
    });

    server.setMessageCallback([&](const mini::net::TcpConnectionPtr& connection, mini::net::Buffer* buffer) {
        ++messageEvents;
        assert(connection->getLoop() == workerLoop);
        assert(connection->getLoop()->isInLoopThread());
        assert(std::this_thread::get_id() == workerThreadId);
        connection->send(buffer->retrieveAllAsString());
    });

    server.start();
    const WorkerInfo worker = workerFuture.get();
    workerLoop = worker.loop;
    workerThreadId = worker.threadId;

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

        const std::string payload = "hello threaded reactor";
        const ssize_t written = ::write(fd, payload.data(), payload.size());
        assert(written == static_cast<ssize_t>(payload.size()));

        char buffer[64] = {};
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        assert(n == static_cast<ssize_t>(payload.size()));
        assert(std::string(buffer, static_cast<std::size_t>(n)) == payload);

        ::close(fd);
    });

    baseLoop.loop();
    client.join();

    assert(messageEvents == 1);
    assert(connectionEvents >= 2);
    assert(disconnectedFuture.wait_for(0s) == std::future_status::ready);

    return 0;
}
