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

std::string makeFrame(int index) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%012d|", index);
    return buf;
}

void clientWorker(uint16_t port, std::size_t expectBytes, const std::string& expected, std::promise<void> done) {
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
        char buf[128];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        received.append(buf, static_cast<std::size_t>(n));
    }

    assert(received == expected);
    ::close(fd);
    done.set_value();
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    const uint16_t port = allocateTestPort();
    constexpr int clientCount = 4;
    constexpr int broadcastCount = 300;

    mini::net::EventLoopThread loopThread;
    auto* baseLoop = loopThread.startLoop();

    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "broadcast-ordered-stress", true);
    auto* serverRaw = server.get();
    server->setThreadNum(2);

    std::promise<void> allConnected;
    auto allConnectedFuture = allConnected.get_future();
    std::atomic<int> connectedCount{0};
    serverRaw->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
        if (!conn->connected()) {
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

    std::string expected;
    expected.reserve(13 * broadcastCount);
    for (int i = 0; i < broadcastCount; ++i) {
        expected += makeFrame(i);
    }

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
            expected.size(),
            expected,
            std::move(receiveSignals.back()));
    }

    assert(allConnectedFuture.wait_for(2s) == std::future_status::ready);

    std::thread sender([&serverRaw, broadcastCount, expected] {
        for (int i = 0; i < broadcastCount; ++i) {
            serverRaw->broadcast(makeFrame(i));
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    sender.join();

    for (auto& future : receiveFutures) {
        assert(future.wait_for(4s) == std::future_status::ready);
    }

    for (auto& client : clients) {
        client.join();
    }

    auto stopped = std::make_shared<std::promise<void>>();
    auto stoppedFuture = stopped->get_future();
    baseLoop->queueInLoop([serverRaw, stopped]() {
        serverRaw->stop();
        stopped->set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);

    auto serverOwner = std::make_shared<std::unique_ptr<mini::net::TcpServer>>(std::move(server));
    auto destroyed = std::make_shared<std::promise<void>>();
    auto destroyedFuture = destroyed->get_future();
    baseLoop->queueInLoop([serverOwner, baseLoop, destroyed] {
        if (serverOwner && *serverOwner) {
            serverOwner->reset();
        }
        baseLoop->quit();
        destroyed->set_value();
    });
    assert(destroyedFuture.wait_for(2s) == std::future_status::ready);

    return 0;
}
