#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <functional>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

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
    const uint16_t port = allocateTestPort();

    mini::net::EventLoop baseLoop;
    mini::net::TcpServer server(&baseLoop, mini::net::InetAddress(port, true), "contract_tcp_server");
    server.setThreadNum(1);

    std::promise<WorkerInfo> workerReady;
    auto workerFuture = workerReady.get_future();
    std::promise<void> connected;
    auto connectedFuture = connected.get_future().share();
    std::promise<void> visibleInBaseMap;
    auto visibleInBaseMapFuture = visibleInBaseMap.get_future();
    std::promise<void> disconnected;
    auto disconnectedFuture = disconnected.get_future().share();
    std::promise<void> removedFromBaseMap;
    auto removedFromBaseMapFuture = removedFromBaseMap.get_future();
    std::promise<void> wrongThreadRejected;
    auto wrongThreadRejectedFuture = wrongThreadRejected.get_future();

    std::atomic<bool> connectedSeen{false};
    std::atomic<bool> disconnectedSeen{false};
    mini::net::EventLoop* workerLoop = nullptr;
    std::thread::id workerThreadId;

    server.setThreadInitCallback([&](mini::net::EventLoop* loop) {
        workerReady.set_value(WorkerInfo{loop, std::this_thread::get_id()});
    });

    server.setConnectionCallback([&](const mini::net::TcpConnectionPtr& connection) {
        if (connection->connected()) {
            assert(connection->getLoop() == workerLoop);
            assert(connection->getLoop()->isInLoopThread());
            assert(std::this_thread::get_id() == workerThreadId);

            if (!connectedSeen.exchange(true)) {
                connected.set_value();
                baseLoop.queueInLoop([&] {
                    assert(server.connectionCount() == 1);
                    visibleInBaseMap.set_value();
                });
            }
            return;
        }

        if (!disconnectedSeen.exchange(true)) {
            disconnected.set_value();
        }
    });

    server.start();
    const WorkerInfo worker = workerFuture.get();
    workerLoop = worker.loop;
    workerThreadId = worker.threadId;

    std::thread wrongThreadProbe([&] {
        assert(connectedFuture.wait_for(1s) == std::future_status::ready);
        try {
            (void)server.connectionCount();
            assert(false);
        } catch (const std::runtime_error&) {
            wrongThreadRejected.set_value();
        }
    });

    std::thread removalProbe([&] {
        assert(disconnectedFuture.wait_for(1s) == std::future_status::ready);

        for (int attempt = 0; attempt < 20; ++attempt) {
            std::promise<std::size_t> countPromise;
            auto countFuture = countPromise.get_future();
            baseLoop.queueInLoop([&server, &countPromise] { countPromise.set_value(server.connectionCount()); });

            assert(countFuture.wait_for(1s) == std::future_status::ready);
            if (countFuture.get() == 0) {
                removedFromBaseMap.set_value();
                baseLoop.queueInLoop([&baseLoop] { baseLoop.quit(); });
                return;
            }

            std::this_thread::sleep_for(10ms);
        }

        assert(false);
    });

    std::thread client([port, connectedFuture] {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        assert(fd >= 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        const int converted = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        assert(converted == 1);

        const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        assert(rc == 0);

        assert(connectedFuture.wait_for(1s) == std::future_status::ready);
        ::close(fd);
    });

    baseLoop.loop();
    client.join();
    wrongThreadProbe.join();
    removalProbe.join();

    assert(visibleInBaseMapFuture.wait_for(0s) == std::future_status::ready);
    assert(disconnectedFuture.wait_for(0s) == std::future_status::ready);
    assert(removedFromBaseMapFuture.wait_for(0s) == std::future_status::ready);
    assert(wrongThreadRejectedFuture.wait_for(0s) == std::future_status::ready);
    assert(server.connectionCount() == 0);

    return 0;
}
