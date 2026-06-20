#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpServer.h"
#include "mini/net/TcpConnection.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

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

void clientWorker(uint16_t port, std::size_t expectBytes, std::promise<void> done) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);

    assert(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

    timeval timeout{};
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    assert(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

    std::string received;
    while (received.size() < expectBytes) {
        char buf[64];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        assert(n > 0);
        received.append(buf, static_cast<std::size_t>(n));
    }

    assert(received.size() == expectBytes);
    ::close(fd);
    done.set_value();
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    const uint16_t port = allocateTestPort();
    constexpr int kClientCount = 4;
    constexpr int kBroadcastCount = 16;
    const std::string payload = "batch-msg|";
    const std::size_t expectedBytes = payload.size() * kBroadcastCount;

    mini::net::EventLoopThread loopThread;
    auto* baseLoop = loopThread.startLoop();

    mini::net::TcpServer server(baseLoop, mini::net::InetAddress(port, true), "broadcast-batch-contract", true);
    server.setThreadNum(2);

    std::atomic<int> connectedCount{0};
    std::promise<void> allConnected;
    auto allConnectedFuture = allConnected.get_future();
    server.setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
        if (!conn->connected()) {
            return;
        }
        if (++connectedCount == kClientCount) {
            allConnected.set_value();
        }
    });

    baseLoop->runInLoop([&server] { server.start(); });
    std::vector<std::promise<void>> clientPromises;
    std::vector<std::future<void>> clientFutures;
    clientPromises.reserve(kClientCount);
    clientFutures.reserve(kClientCount);
    std::vector<std::thread> clients;
    clients.reserve(kClientCount);

    for (int i = 0; i < kClientCount; ++i) {
        clientPromises.emplace_back();
        clientFutures.push_back(clientPromises.back().get_future());
        clients.emplace_back(
            clientWorker,
            port,
            expectedBytes,
            std::move(clientPromises.back()));
    }

    assert(allConnectedFuture.wait_for(2s) == std::future_status::ready);

    baseLoop->queueInLoop([&server, &payload] {
        for (int i = 0; i < kBroadcastCount; ++i) {
            server.broadcast(payload);
        }
    });

    for (auto& future : clientFutures) {
        assert(future.wait_for(2s) == std::future_status::ready);
    }

    for (auto& client : clients) {
        client.join();
    }

    baseLoop->queueInLoop([&server] { server.stop(); });
    baseLoop->queueInLoop([baseLoop] { baseLoop->quit(); });
    return 0;
}
