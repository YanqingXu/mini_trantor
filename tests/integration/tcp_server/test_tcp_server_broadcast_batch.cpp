#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
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
    const int clientCount = 3;
    const int broadcastCount = 12;
    const std::string payload = "integration-batch";
    const std::size_t expectedBytes = payload.size() * broadcastCount;

    mini::net::EventLoopThread loopThread;
    auto* baseLoop = loopThread.startLoop();

    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "broadcast-batch-integrated", true);
    auto* serverRaw = server.get();
    serverRaw->setThreadNum(2);

    std::promise<void> allConnected;
    auto allConnectedFuture = allConnected.get_future();
    std::promise<void> allDisconnected;
    auto allDisconnectedFuture = allDisconnected.get_future();
    std::atomic<int> connectedCount{0};
    std::atomic<int> disconnectedCount{0};
    serverRaw->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
        if (!conn->connected()) {
            if (disconnectedCount.fetch_add(1) == clientCount - 1) {
                allDisconnected.set_value();
            }
            return;
        }

        if (++connectedCount == clientCount) {
            allConnected.set_value();
        }
    });

    auto started = std::make_shared<std::promise<void>>();
    auto startedFuture = started->get_future();
    baseLoop->queueInLoop([serverRaw, started] {
        serverRaw->start();
        started->set_value();
    });
    assert(startedFuture.wait_for(2s) == std::future_status::ready);

    std::vector<std::thread> clients;
    std::vector<std::promise<void>> receiveSignals;
    std::vector<std::future<void>> receiveFutures;
    receiveSignals.reserve(clientCount);
    receiveFutures.reserve(clientCount);

    for (int i = 0; i < clientCount; ++i) {
        receiveSignals.emplace_back();
        receiveFutures.push_back(receiveSignals.back().get_future());
        clients.emplace_back(
            clientWorker,
            port,
            expectedBytes,
            std::move(receiveSignals.back()));
    }

    assert(allConnectedFuture.wait_for(3s) == std::future_status::ready);

    baseLoop->queueInLoop([serverRaw, payload, broadcastCount] {
        for (int i = 0; i < broadcastCount; ++i) {
            serverRaw->broadcast(payload);
        }
    });

    for (auto& future : receiveFutures) {
        assert(future.wait_for(3s) == std::future_status::ready);
    }

    for (auto& client : clients) {
        client.join();
    }

    assert(allDisconnectedFuture.wait_for(3s) == std::future_status::ready);

    auto stopped = std::make_shared<std::promise<void>>();
    auto stoppedFuture = stopped->get_future();
    baseLoop->queueInLoop([serverRaw, stopped]() {
        serverRaw->stop();
        stopped->set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);

    auto serverOwner = std::make_shared<std::unique_ptr<mini::net::TcpServer>>(std::move(server));
    auto destroyDone = std::make_shared<std::promise<void>>();
    auto destroyDoneFuture = destroyDone->get_future();
    baseLoop->queueInLoop([serverOwner, baseLoop, destroyDone]() {
        if (serverOwner && *serverOwner) {
            serverOwner->reset();
        }
        baseLoop->quit();
        destroyDone->set_value();
    });
    assert(destroyDoneFuture.wait_for(2s) == std::future_status::ready);
    return 0;
}
