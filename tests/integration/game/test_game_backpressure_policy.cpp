#include "mini/game/GameServerPipeline.h"
#include "mini/game/SessionManager.h"
#include "mini/game/logic/LogicLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpServer.h"
#include "mini/net/TcpServerOptions.h"
#include "mini/net/framing/PacketFramer.h"
#include "mini/net/transport/TransportManager.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
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
    assert(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const auto port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

int connectClient(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    assert(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    return fd;
}

bool waitForClose(int fd, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char byte{};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto n = ::recv(fd, &byte, sizeof(byte), MSG_DONTWAIT);
        if (n == 0) {
            return true;
        }
        if (n < 0 && errno == ECONNRESET) {
            return true;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            std::this_thread::sleep_for(5ms);
            continue;
        }
        if (n < 0) {
            return true;
        }
    }
    return false;
}

struct DecodedFrame {
    std::uint32_t msgId{0};
    std::uint32_t seq{0};
    std::string payload;
};

class FramedClient {
public:
    explicit FramedClient(uint16_t port) {
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

    ~FramedClient() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    void sendFrame(std::uint32_t msgId,
                   std::uint32_t seq,
                   std::string_view payload,
                   std::uint16_t flags = 0) {
        const auto frame = framer_.encode(msgId, flags, seq, payload);
        assert(::send(fd_, frame.data(), frame.size(), 0) == static_cast<ssize_t>(frame.size()));
    }

    DecodedFrame readFrame() {
        for (;;) {
            mini::net::framing::Packet packet;
            std::size_t consumed = 0;
            const auto state = framer_.decode(input_.data(), input_.size(), packet, consumed);
            if (state == mini::net::framing::PacketDecodeState::kComplete) {
                DecodedFrame decoded{
                    packet.header.msgId,
                    packet.header.seq,
                    std::string(packet.payload)};
                input_.erase(0, consumed);
                return decoded;
            }
            assert(state == mini::net::framing::PacketDecodeState::kNeedMore);

            char buffer[256]{};
            const auto n = ::recv(fd_, buffer, sizeof(buffer), 0);
            assert(n > 0);
            input_.append(buffer, static_cast<std::size_t>(n));
        }
    }

    bool expectNoData(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        char byte{};
        while (std::chrono::steady_clock::now() < deadline) {
            const auto n = ::recv(fd_, &byte, sizeof(byte), MSG_DONTWAIT | MSG_PEEK);
            if (n > 0) {
                return false;
            }
            if (n == 0) {
                return false;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                std::this_thread::sleep_for(5ms);
                continue;
            }
            return false;
        }
        return true;
    }

private:
    int fd_{-1};
    mini::net::framing::PacketFramer framer_;
    std::string input_;
};

void stopAndDestroyServer(mini::net::EventLoop* baseLoop,
                          mini::net::TcpServer* serverRaw,
                          std::unique_ptr<mini::net::TcpServer> server) {
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
}

void waitForBaseLoopBarrier(mini::net::EventLoop* baseLoop) {
    auto barrier = std::make_shared<std::promise<void>>();
    auto barrierFuture = barrier->get_future();
    baseLoop->queueInLoop([barrier] { barrier->set_value(); });
    assert(barrierFuture.wait_for(2s) == std::future_status::ready);
}

void testInputHardLimitClosesConnection() {
    const auto port = allocateTestPort();

    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();

    mini::net::transport::TransportManager transportManager(baseLoop);
    mini::game::SessionManager sessionManager(baseLoop);
    mini::game::logic::LogicLoop logicLoop({.fixedStep = 20ms, .maxCommandsPerTick = 4});

    mini::net::TcpServerOptions serverOptions;
    serverOptions.numThreads = 1;
    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop,
        mini::net::InetAddress(port, true),
        "game-backpressure-input",
        serverOptions);
    auto* serverRaw = server.get();

    mini::game::GameServerPipeline::Options pipelineOptions;
    pipelineOptions.backpressure.input.softBufferedBytes = 12;
    pipelineOptions.backpressure.input.hardBufferedBytes = 20;

    mini::game::GameServerPipeline pipeline(
        *serverRaw,
        transportManager,
        sessionManager,
        logicLoop,
        pipelineOptions);

    std::promise<void> rejectedMetricPromise;
    auto rejectedMetricFuture = rejectedMetricPromise.get_future();
    std::once_flag rejectedOnce;
    pipeline.setBackpressureMetricCallback(
        [&](const mini::game::GameBackpressureMetricSample& sample) {
            assert(sample.loop != nullptr);
            if (sample.event != mini::game::GameBackpressureMetricEvent::InputRejected) {
                return;
            }
            assert(sample.layer == mini::game::GameBackpressureLayer::InputFraming);
            assert(sample.action == mini::game::GameBackpressureAction::Close);
            assert(sample.reason == mini::game::GameBackpressureReason::InputBufferedBytesHardLimit);
            assert(sample.currentValue > sample.hardLimit);
            assert(sample.softLimit == 12);
            assert(sample.hardLimit == 20);
            std::call_once(rejectedOnce, [&] { rejectedMetricPromise.set_value(); });
        });
    pipeline.install();

    auto started = std::make_shared<std::promise<void>>();
    auto startedFuture = started->get_future();
    baseLoop->queueInLoop([serverRaw, started] {
        serverRaw->start();
        started->set_value();
    });
    assert(startedFuture.wait_for(2s) == std::future_status::ready);

    const int fd = connectClient(port);

    mini::net::framing::PacketFramer framer;
    const auto fullFrame = framer.encode(2, 0, 1, std::string(64, 'x'));
    assert(fullFrame.size() > 24);
    const std::string partialFrame = fullFrame.substr(0, 24);
    assert(::send(fd,
                  partialFrame.data(),
                  partialFrame.size(),
                  0) == static_cast<ssize_t>(partialFrame.size()));

    assert(rejectedMetricFuture.wait_for(2s) == std::future_status::ready);
    assert(waitForClose(fd, 2s));
    ::close(fd);

    stopAndDestroyServer(baseLoop, serverRaw, std::move(server));
}

void testBroadcastHardPayloadRejectsDispatch() {
    const auto port = allocateTestPort();

    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();

    mini::net::transport::TransportManager transportManager(baseLoop);
    mini::game::SessionManager sessionManager(baseLoop);
    mini::game::logic::LogicLoop logicLoop({.fixedStep = 20ms, .maxCommandsPerTick = 4});

    mini::net::TcpServerOptions serverOptions;
    serverOptions.numThreads = 1;
    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop,
        mini::net::InetAddress(port, true),
        "game-backpressure-broadcast",
        serverOptions);
    auto* serverRaw = server.get();

    mini::game::GameServerPipeline::Options pipelineOptions;
    pipelineOptions.backpressure.broadcast.softPayloadBytes = 4;
    pipelineOptions.backpressure.broadcast.hardPayloadBytes = 8;

    mini::game::GameServerPipeline pipeline(
        *serverRaw,
        transportManager,
        sessionManager,
        logicLoop,
        pipelineOptions);

    std::promise<void> rejectedMetricPromise;
    auto rejectedMetricFuture = rejectedMetricPromise.get_future();
    std::once_flag rejectedOnce;
    pipeline.setBackpressureMetricCallback(
        [&](const mini::game::GameBackpressureMetricSample& sample) {
            assert(sample.loop != nullptr);
            if (sample.event != mini::game::GameBackpressureMetricEvent::BroadcastRejected) {
                return;
            }
            assert(sample.layer == mini::game::GameBackpressureLayer::BroadcastFanout);
            assert(sample.action == mini::game::GameBackpressureAction::Reject);
            assert(sample.reason == mini::game::GameBackpressureReason::BroadcastPayloadBytesHardLimit);
            assert(sample.payloadBytes > sample.hardLimit);
            assert(sample.softLimit == 4);
            assert(sample.hardLimit == 8);
            std::call_once(rejectedOnce, [&] { rejectedMetricPromise.set_value(); });
        });
    pipeline.install();

    auto started = std::make_shared<std::promise<void>>();
    auto startedFuture = started->get_future();
    baseLoop->queueInLoop([serverRaw, started] {
        serverRaw->start();
        started->set_value();
    });
    assert(startedFuture.wait_for(2s) == std::future_status::ready);

    FramedClient client(port);
    client.sendFrame(1, 1, "player-broadcast-hard");
    const auto auth = client.readFrame();
    assert(auth.msgId == 4);
    assert(auth.seq == 1);
    assert(auth.payload == "auth-ok");

    waitForBaseLoopBarrier(baseLoop);

    client.sendFrame(3, 2, "hello-world");
    assert(rejectedMetricFuture.wait_for(2s) == std::future_status::ready);
    assert(client.expectNoData(150ms));

    stopAndDestroyServer(baseLoop, serverRaw, std::move(server));
}

void testBroadcastSoftFanoutDropsOnlyLowPriority() {
    const auto port = allocateTestPort();

    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();

    mini::net::transport::TransportManager transportManager(baseLoop);
    mini::game::SessionManager sessionManager(baseLoop);
    mini::game::logic::LogicLoop logicLoop({.fixedStep = 20ms, .maxCommandsPerTick = 4});

    mini::net::TcpServerOptions serverOptions;
    serverOptions.numThreads = 1;
    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop,
        mini::net::InetAddress(port, true),
        "game-backpressure-broadcast-priority",
        serverOptions);
    auto* serverRaw = server.get();

    mini::game::GameServerPipeline::Options pipelineOptions;
    pipelineOptions.backpressure.broadcast.softFanoutConnections = 1;
    pipelineOptions.backpressure.broadcast.hardFanoutConnections = 8;
    pipelineOptions.backpressure.broadcast.priority.softLimitMinPriority =
        mini::game::GameMessagePriority::Normal;

    mini::game::GameServerPipeline pipeline(
        *serverRaw,
        transportManager,
        sessionManager,
        logicLoop,
        pipelineOptions);

    std::promise<void> droppedMetricPromise;
    auto droppedMetricFuture = droppedMetricPromise.get_future();
    std::once_flag droppedOnce;
    pipeline.setBackpressureMetricCallback(
        [&](const mini::game::GameBackpressureMetricSample& sample) {
            assert(sample.loop != nullptr);
            if (sample.event != mini::game::GameBackpressureMetricEvent::BroadcastRejected ||
                sample.action != mini::game::GameBackpressureAction::DropLowPriority) {
                return;
            }
            assert(sample.layer == mini::game::GameBackpressureLayer::BroadcastFanout);
            assert(sample.reason == mini::game::GameBackpressureReason::BroadcastFanoutSoftLimit);
            assert(sample.priority == mini::game::toMetricPriority(mini::game::GameMessagePriority::Low));
            assert(sample.currentValue == 1);
            assert(sample.softLimit == 1);
            assert(sample.hardLimit == 8);
            std::call_once(droppedOnce, [&] { droppedMetricPromise.set_value(); });
        });
    pipeline.install();

    auto started = std::make_shared<std::promise<void>>();
    auto startedFuture = started->get_future();
    baseLoop->queueInLoop([serverRaw, started] {
        serverRaw->start();
        started->set_value();
    });
    assert(startedFuture.wait_for(2s) == std::future_status::ready);

    FramedClient client(port);
    client.sendFrame(1, 1, "player-broadcast-priority");
    const auto auth = client.readFrame();
    assert(auth.msgId == 4);
    assert(auth.seq == 1);
    assert(auth.payload == "auth-ok");

    waitForBaseLoopBarrier(baseLoop);

    client.sendFrame(3, 2, "low", mini::game::kGamePriorityFlagLow);
    assert(droppedMetricFuture.wait_for(2s) == std::future_status::ready);
    assert(client.expectNoData(150ms));

    client.sendFrame(3, 3, "high", mini::game::kGamePriorityFlagHigh);
    const auto high = client.readFrame();
    assert(high.msgId == 4);
    assert(high.seq == 3);
    assert(high.payload == "broadcast:high");

    stopAndDestroyServer(baseLoop, serverRaw, std::move(server));
}

}  // namespace

int main() {
    testInputHardLimitClosesConnection();
    testBroadcastHardPayloadRejectsDispatch();
    testBroadcastSoftFanoutDropsOnlyLowPriority();
    return 0;
}
