#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"
#include "mini/net/TcpServerOptions.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr auto kRouteLatencyThreshold = std::chrono::milliseconds(500);
constexpr auto kQueueLatencyThreshold = std::chrono::milliseconds(500);
constexpr auto kFanoutLatencyThreshold = std::chrono::seconds(1);

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

uint16_t allocateTestPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    require(fd >= 0, "failed to create test socket");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    require(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0,
            "failed to bind test socket");

    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    require(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0,
            "failed to read test socket port");

    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

void clientWorker(uint16_t port, std::size_t expectBytes, std::promise<void> done) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    require(fd >= 0, "client socket failed");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    require(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1,
            "client address conversion failed");
    require(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0,
            "client connect failed");

    timeval timeout{};
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    require(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
            "client receive timeout setup failed");

    std::string received;
    received.reserve(expectBytes);
    while (received.size() < expectBytes) {
        char buffer[128];
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        require(n > 0, "client receive failed before expected bytes arrived");
        received.append(buffer, static_cast<std::size_t>(n));
    }

    require(received.size() == expectBytes, "client received unexpected byte count");
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
    mini::net::BroadcastMetricSample::Duration maxRouteLatency{};
    mini::net::BroadcastMetricSample::Duration maxQueueLatency{};
    mini::net::BroadcastMetricSample::Duration maxFanoutLatency{};

    serverRaw->setBroadcastMetricCallback(
        [&](const mini::net::BroadcastMetricSample& sample) {
            std::lock_guard lock(metricsMutex);
            if (sample.event == mini::net::BroadcastMetricEvent::Routed) {
                require(sample.loop == baseLoop, "routed metric loop mismatch");
                require(sample.payloadBytes == payload.size(), "routed metric payload size mismatch");
                require(sample.fanoutConnections == static_cast<std::size_t>(kClientCount),
                        "routed metric fanout mismatch");
                require(sample.routeLatency >= mini::net::BroadcastMetricSample::Duration::zero(),
                        "routed metric latency is negative");
                maxRouteLatency = std::max(maxRouteLatency, sample.routeLatency);
                ++routedSamples;
                payloadSizeObserved = true;
                fanoutObserved = true;
            } else if (sample.event == mini::net::BroadcastMetricEvent::LoopFlushed) {
                require(sample.payloadBytes == payload.size(), "flushed metric payload size mismatch");
                require(sample.fanoutConnections > 0, "flushed metric fanout is empty");
                require(sample.queueLatency >= mini::net::BroadcastMetricSample::Duration::zero(),
                        "flushed metric queue latency is negative");
                require(sample.fanoutLatency >= mini::net::BroadcastMetricSample::Duration::zero(),
                        "flushed metric fanout latency is negative");
                maxQueueLatency = std::max(maxQueueLatency, sample.queueLatency);
                maxFanoutLatency = std::max(maxFanoutLatency, sample.fanoutLatency);
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
    require(startedFuture.wait_for(2s) == std::future_status::ready,
            "server did not start before timeout");

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

    require(allConnectedFuture.wait_for(3s) == std::future_status::ready,
            "clients did not connect before timeout");

    baseLoop->queueInLoop([serverRaw, payload] {
        for (int i = 0; i < kBroadcastCount; ++i) {
            serverRaw->broadcast(payload);
        }
    });

    for (auto& future : clientFutures) {
        require(future.wait_for(3s) == std::future_status::ready,
                "client did not receive expected broadcast bytes before timeout");
    }
    for (auto& client : clients) {
        client.join();
    }

    {
        std::unique_lock lock(metricsMutex);
        require(metricsCv.wait_for(lock, 2s, [&] {
                    return routedSamples >= kBroadcastCount &&
                           flushedSamples >= kBroadcastCount &&
                           payloadSizeObserved &&
                           fanoutObserved &&
                           latencyObserved;
                }),
                "broadcast metrics did not arrive before timeout");
        require(maxRouteLatency <= kRouteLatencyThreshold, "route latency exceeded threshold");
        require(maxQueueLatency <= kQueueLatencyThreshold, "queue latency exceeded threshold");
        require(maxFanoutLatency <= kFanoutLatencyThreshold, "fanout latency exceeded threshold");
    }

    auto stopped = std::make_shared<std::promise<void>>();
    auto stoppedFuture = stopped->get_future();
    baseLoop->queueInLoop([serverRaw, stopped] {
        serverRaw->stop();
        stopped->set_value();
    });
    require(stoppedFuture.wait_for(2s) == std::future_status::ready,
            "server did not stop before timeout");

    auto serverOwner = std::make_shared<std::unique_ptr<mini::net::TcpServer>>(std::move(server));
    auto destroyed = std::make_shared<std::promise<void>>();
    auto destroyedFuture = destroyed->get_future();
    baseLoop->queueInLoop([serverOwner, baseLoop, destroyed] {
        serverOwner->reset();
        baseLoop->quit();
        destroyed->set_value();
    });
    require(destroyedFuture.wait_for(2s) == std::future_status::ready,
            "server did not destroy before timeout");

    return 0;
}
