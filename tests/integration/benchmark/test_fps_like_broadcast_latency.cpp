#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"
#include "mini/net/TcpServerOptions.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
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

    assert(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);

    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

void clientWorker(uint16_t port, std::size_t expectBytes, std::promise<void> done) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
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
    received.reserve(expectBytes);
    while (received.size() < expectBytes) {
        char buffer[128];
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        assert(n > 0);
        received.append(buffer, static_cast<std::size_t>(n));
    }

    assert(received.size() == expectBytes);
    ::close(fd);
    done.set_value();
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    constexpr int kClientCount = 4;
    constexpr int kBroadcastCount = 24;
    const std::string payload = "frame-payload|";
    const std::size_t expectedBytes = payload.size() * kBroadcastCount;
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    auto* baseLoop = loopThread.startLoop();

    mini::net::TcpServerOptions options;
    options.numThreads = 2;
    options.metrics.enableBroadcastMetrics = true;
    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop,
        mini::net::InetAddress(port, true),
        "fps-like-broadcast-latency",
        options);
    auto* serverRaw = server.get();

    std::mutex metricsMutex;
    std::condition_variable metricsCv;
    int routedSamples = 0;
    int flushedSamples = 0;
    bool payloadSizeObserved = false;
    bool fanoutObserved = false;
    bool latencyObserved = false;

    serverRaw->setBroadcastMetricCallback(
        [&](const mini::net::BroadcastMetricSample& sample) {
            std::lock_guard lock(metricsMutex);
            if (sample.event == mini::net::BroadcastMetricEvent::Routed) {
                assert(sample.loop == baseLoop);
                assert(sample.payloadBytes == payload.size());
                assert(sample.fanoutConnections == static_cast<std::size_t>(kClientCount));
                assert(sample.routeLatency >= mini::net::BroadcastMetricSample::Duration::zero());
                ++routedSamples;
                payloadSizeObserved = true;
                fanoutObserved = true;
            } else if (sample.event == mini::net::BroadcastMetricEvent::LoopFlushed) {
                assert(sample.payloadBytes == payload.size());
                assert(sample.fanoutConnections > 0);
                assert(sample.queueLatency >= mini::net::BroadcastMetricSample::Duration::zero());
                assert(sample.fanoutLatency >= mini::net::BroadcastMetricSample::Duration::zero());
                ++flushedSamples;
                latencyObserved = true;
            }
            metricsCv.notify_all();
        });

    std::atomic<int> connectedCount{0};
    auto allConnected = std::make_shared<std::promise<void>>();
    auto allConnectedFuture = allConnected->get_future();
    serverRaw->setConnectionCallback([&](const mini::net::TcpConnectionPtr& connection) {
        if (!connection->connected()) {
            return;
        }
        if (connectedCount.fetch_add(1) + 1 == kClientCount) {
            allConnected->set_value();
        }
    });

    auto started = std::make_shared<std::promise<void>>();
    auto startedFuture = started->get_future();
    baseLoop->queueInLoop([serverRaw, started] {
        serverRaw->start();
        started->set_value();
    });
    assert(startedFuture.wait_for(2s) == std::future_status::ready);

    std::vector<std::promise<void>> clientPromises;
    std::vector<std::future<void>> clientFutures;
    std::vector<std::thread> clients;
    clientPromises.reserve(kClientCount);
    clientFutures.reserve(kClientCount);
    clients.reserve(kClientCount);

    for (int i = 0; i < kClientCount; ++i) {
        clientPromises.emplace_back();
        clientFutures.push_back(clientPromises.back().get_future());
        clients.emplace_back(clientWorker, port, expectedBytes, std::move(clientPromises.back()));
    }

    assert(allConnectedFuture.wait_for(3s) == std::future_status::ready);

    baseLoop->queueInLoop([serverRaw, payload] {
        for (int i = 0; i < kBroadcastCount; ++i) {
            serverRaw->broadcast(payload);
        }
    });

    for (auto& future : clientFutures) {
        assert(future.wait_for(3s) == std::future_status::ready);
    }
    for (auto& client : clients) {
        client.join();
    }

    {
        std::unique_lock lock(metricsMutex);
        assert(metricsCv.wait_for(lock, 2s, [&] {
            return routedSamples >= kBroadcastCount &&
                   flushedSamples >= kBroadcastCount &&
                   payloadSizeObserved &&
                   fanoutObserved &&
                   latencyObserved;
        }));
    }

    auto stopped = std::make_shared<std::promise<void>>();
    auto stoppedFuture = stopped->get_future();
    baseLoop->queueInLoop([serverRaw, stopped] {
        serverRaw->stop();
        stopped->set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);

    auto serverOwner = std::make_shared<std::unique_ptr<mini::net::TcpServer>>(std::move(server));
    auto destroyed = std::make_shared<std::promise<void>>();
    auto destroyedFuture = destroyed->get_future();
    baseLoop->queueInLoop([serverOwner, baseLoop, destroyed] {
        serverOwner->reset();
        baseLoop->quit();
        destroyed->set_value();
    });
    assert(destroyedFuture.wait_for(2s) == std::future_status::ready);

    return 0;
}
