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
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

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

void clientWorker(uint16_t port, const std::string& expectPayload, std::promise<void> done) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);

    assert(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

    timeval timeout{};
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    assert(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

    std::string received;
    while (received.size() < expectPayload.size()) {
        char buffer[32]{};
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
        received.append(buffer, buffer + static_cast<std::size_t>(n));
    }

    assert(received == expectPayload);
    ::close(fd);
    done.set_value();
}

}  // namespace

int main() {
    const uint16_t port = allocateTestPort();
    const int clientCount = 3;

    mini::net::EventLoopThread loopThread;
    auto* baseLoop = loopThread.startLoop();

    mini::net::TcpServer server(baseLoop, mini::net::InetAddress(port, true), "broadcast-threaded", true);
    server.setThreadNum(2);

    std::atomic<int> connectedCount{0};
    std::mutex sessionIdsMu;
    std::vector<std::string> sessionIds;
    std::promise<std::vector<std::string>> allConnected;
    auto allConnectedFuture = allConnected.get_future();

    server.setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
        if (!conn->connected()) {
            return;
        }

        std::lock_guard<std::mutex> lock(sessionIdsMu);
        sessionIds.push_back(conn->name());

        const int current = ++connectedCount;
        if (current == clientCount) {
            allConnected.set_value(sessionIds);
        }
    });

    std::promise<void> started;
    auto startedFuture = started.get_future();
    baseLoop->queueInLoop([&] {
        server.start();
        started.set_value();
    });
    assert(startedFuture.wait_for(2s) == std::future_status::ready);

    std::vector<std::promise<void>> receivePromises(clientCount);
    std::vector<std::future<void>> receiveFutures;
    receiveFutures.reserve(clientCount);
    const std::string payload = "broadcast-threaded";
    for (int i = 0; i < clientCount; ++i) {
        receiveFutures.push_back(receivePromises[i].get_future());
    }

    std::vector<std::thread> clients;
    clients.reserve(clientCount);
    for (int i = 0; i < clientCount; ++i) {
        clients.emplace_back(clientWorker, port, payload, std::move(receivePromises[i]));
    }

    const auto sessions = allConnectedFuture.get();
    assert(sessions.size() == static_cast<std::size_t>(clientCount));

    baseLoop->queueInLoop([&server, payload] { server.broadcast(payload); });

    for (auto& future : receiveFutures) {
        assert(future.wait_for(2s) == std::future_status::ready);
    }

    for (auto& client : clients) {
        client.join();
    }

    auto stopped = std::make_shared<std::promise<void>>();
    auto stoppedFuture = stopped->get_future();
    baseLoop->queueInLoop([&server, stopped]() {
        server.stop();
        stopped->set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);

    baseLoop->queueInLoop([&] { baseLoop->quit(); });
    return 0;
}
