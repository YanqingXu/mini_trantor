#include "mini/net/Buffer.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"
#include "mini/net/TcpServerOptions.h"

#include <algorithm>
#include <any>
#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

constexpr auto kQueueLatencyThreshold = std::chrono::milliseconds(500);
constexpr auto kFanoutLatencyThreshold = std::chrono::seconds(1);
constexpr auto kRouteLatencyThreshold = std::chrono::milliseconds(500);

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
    const auto port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

class RawClient {
public:
    explicit RawClient(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        assert(fd_ >= 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        assert(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
        assert(::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

        timeval timeout{};
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;
        assert(::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    }

    ~RawClient() {
        closeNow();
    }

    void auth(std::string_view token) {
        assert(::send(fd_, token.data(), token.size(), 0) == static_cast<ssize_t>(token.size()));
        assert(readExact(2) == "ok");
    }

    std::string readExact(std::size_t size) {
        std::string out;
        out.reserve(size);
        while (out.size() < size) {
            char buffer[64]{};
            const auto n = ::recv(fd_, buffer, sizeof(buffer), 0);
            assert(n > 0);
            out.append(buffer, static_cast<std::size_t>(n));
        }
        return out;
    }

    void expectNoData(std::chrono::milliseconds timeout) {
        pollfd pfd{fd_, POLLIN, 0};
        const auto ready = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
        assert(ready == 0);
    }

    void closeNow() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_{-1};
};

void waitBaseLoop(mini::net::EventLoop* loop) {
    auto ready = std::make_shared<std::promise<void>>();
    auto future = ready->get_future();
    loop->queueInLoop([ready] { ready->set_value(); });
    assert(future.wait_for(2s) == std::future_status::ready);
}

}  // namespace

int main() {
    const auto port = allocateTestPort();
    const std::string roomPayloadDuringReconnect = "room-during-reconnect|";
    const std::string roomPayloadAfterReconnect = "room-after-reconnect|";
    const std::string aoiPayload = "aoi-east|";

    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();

    mini::net::TcpServerOptions options;
    options.numThreads = 2;
    options.metrics.enableBroadcastMetrics = true;
    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop,
        mini::net::InetAddress(port, true),
        "broadcast-session-group-aoi",
        options);
    auto* serverRaw = server.get();

    std::mutex stateMutex;
    std::condition_variable stateCv;
    int authedCount = 0;
    bool player2Disconnected = false;

    std::mutex metricsMutex;
    std::condition_variable metricsCv;
    int routedSamples = 0;
    int flushedSamples = 0;
    bool sawFanout2DuringReconnect = false;
    bool sawFanout3AfterReconnect = false;
    bool sawAoiFanout1 = false;
    bool sawPayloadBytes = false;
    mini::net::BroadcastMetricSample::Duration maxQueueLatency{};
    mini::net::BroadcastMetricSample::Duration maxFanoutLatency{};
    mini::net::BroadcastMetricSample::Duration maxRouteLatency{};

    serverRaw->setBroadcastMetricCallback([&](const mini::net::BroadcastMetricSample& sample) {
        std::lock_guard lock(metricsMutex);
        if (sample.event == mini::net::BroadcastMetricEvent::Routed) {
            assert(sample.loop == baseLoop);
            assert(sample.targeted);
            assert(sample.requestedSessions == sample.fanoutConnections);
            assert(sample.routeLatency >= mini::net::BroadcastMetricSample::Duration::zero());
            maxRouteLatency = std::max(maxRouteLatency, sample.routeLatency);
            sawPayloadBytes = sawPayloadBytes ||
                sample.payloadBytes == roomPayloadDuringReconnect.size() ||
                sample.payloadBytes == roomPayloadAfterReconnect.size() ||
                sample.payloadBytes == aoiPayload.size();
            if (sample.payloadBytes == roomPayloadDuringReconnect.size()) {
                assert(sample.fanoutConnections == 2);
                sawFanout2DuringReconnect = true;
            } else if (sample.payloadBytes == roomPayloadAfterReconnect.size()) {
                assert(sample.fanoutConnections == 3);
                sawFanout3AfterReconnect = true;
            } else if (sample.payloadBytes == aoiPayload.size()) {
                assert(sample.fanoutConnections == 1);
                sawAoiFanout1 = true;
            }
            ++routedSamples;
        } else if (sample.event == mini::net::BroadcastMetricEvent::LoopFlushed) {
            assert(sample.payloadBytes > 0);
            assert(sample.fanoutConnections > 0);
            assert(sample.queueLatency >= mini::net::BroadcastMetricSample::Duration::zero());
            assert(sample.fanoutLatency >= mini::net::BroadcastMetricSample::Duration::zero());
            maxQueueLatency = std::max(maxQueueLatency, sample.queueLatency);
            maxFanoutLatency = std::max(maxFanoutLatency, sample.fanoutLatency);
            ++flushedSamples;
        }
        metricsCv.notify_all();
    });

    serverRaw->setConnectionCallback([&](const mini::net::TcpConnectionPtr& connection) {
        if (connection->connected()) {
            return;
        }

        auto* token = std::any_cast<std::string>(&connection->getContext());
        if (!token || token->empty()) {
            return;
        }

        serverRaw->unbindBroadcastSession(*token);
        if (*token == "player-2") {
            {
                std::lock_guard lock(stateMutex);
                player2Disconnected = true;
            }
            stateCv.notify_all();
        }
    });

    serverRaw->setMessageCallback([&](const mini::net::TcpConnectionPtr& connection,
                                      mini::net::Buffer* buffer) {
        const auto token = buffer->retrieveAllAsString();
        assert(!token.empty());

        connection->setContext(token);
        serverRaw->bindBroadcastSession(connection, token);
        serverRaw->joinBroadcastGroup(token, "room-1");
        if (token == "player-0") {
            serverRaw->joinBroadcastAoi(token, "aoi-east");
        }
        connection->send("ok");

        {
            std::lock_guard lock(stateMutex);
            ++authedCount;
        }
        stateCv.notify_all();
    });

    auto started = std::make_shared<std::promise<void>>();
    auto startedFuture = started->get_future();
    baseLoop->queueInLoop([serverRaw, started] {
        serverRaw->start();
        started->set_value();
    });
    assert(startedFuture.wait_for(2s) == std::future_status::ready);

    RawClient player0(port);
    RawClient player1(port);
    {
        RawClient player2(port);
        player0.auth("player-0");
        player1.auth("player-1");
        player2.auth("player-2");
        {
            std::unique_lock lock(stateMutex);
            assert(stateCv.wait_for(lock, 2s, [&] { return authedCount == 3; }));
        }
        waitBaseLoop(baseLoop);

        player2.closeNow();
    }
    {
        std::unique_lock lock(stateMutex);
        assert(stateCv.wait_for(lock, 2s, [&] { return player2Disconnected; }));
    }
    waitBaseLoop(baseLoop);

    serverRaw->broadcastGroup("room-1", roomPayloadDuringReconnect);
    assert(player0.readExact(roomPayloadDuringReconnect.size()) == roomPayloadDuringReconnect);
    assert(player1.readExact(roomPayloadDuringReconnect.size()) == roomPayloadDuringReconnect);

    RawClient player2Reconnect(port);
    player2Reconnect.auth("player-2");
    {
        std::unique_lock lock(stateMutex);
        assert(stateCv.wait_for(lock, 2s, [&] { return authedCount == 4; }));
    }
    waitBaseLoop(baseLoop);

    serverRaw->broadcastGroup("room-1", roomPayloadAfterReconnect);
    assert(player0.readExact(roomPayloadAfterReconnect.size()) == roomPayloadAfterReconnect);
    assert(player1.readExact(roomPayloadAfterReconnect.size()) == roomPayloadAfterReconnect);
    assert(player2Reconnect.readExact(roomPayloadAfterReconnect.size()) == roomPayloadAfterReconnect);

    serverRaw->broadcastAoi("aoi-east", aoiPayload);
    assert(player0.readExact(aoiPayload.size()) == aoiPayload);
    player1.expectNoData(100ms);
    player2Reconnect.expectNoData(100ms);

    {
        std::unique_lock lock(metricsMutex);
        assert(metricsCv.wait_for(lock, 2s, [&] {
            return routedSamples >= 3 &&
                   flushedSamples >= 3 &&
                   sawFanout2DuringReconnect &&
                   sawFanout3AfterReconnect &&
                   sawAoiFanout1 &&
                   sawPayloadBytes;
        }));
        assert(maxRouteLatency <= kRouteLatencyThreshold);
        assert(maxQueueLatency <= kQueueLatencyThreshold);
        assert(maxFanoutLatency <= kFanoutLatencyThreshold);
    }

    player0.closeNow();
    player1.closeNow();
    player2Reconnect.closeNow();

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
