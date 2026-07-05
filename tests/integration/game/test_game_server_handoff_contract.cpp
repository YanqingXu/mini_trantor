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
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <set>
#include <string>
#include <string_view>
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
    assert(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const auto port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
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
        close();
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    void sendFrame(std::uint32_t msgId, std::uint32_t seq, std::string_view payload) {
        const auto frame = framer_.encode(msgId, 0, seq, payload);
        assert(::send(fd_, frame.data(), frame.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(frame.size()));
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

            char buffer[512]{};
            const auto n = ::recv(fd_, buffer, sizeof(buffer), 0);
            assert(n > 0);
            input_.append(buffer, static_cast<std::size_t>(n));
        }
    }

private:
    int fd_{-1};
    mini::net::framing::PacketFramer framer_;
    std::string input_;
};

void waitForBaseLoopBarrier(mini::net::EventLoop* baseLoop) {
    auto barrier = std::make_shared<std::promise<void>>();
    auto barrierFuture = barrier->get_future();
    baseLoop->queueInLoop([barrier] { barrier->set_value(); });
    assert(barrierFuture.wait_for(2s) == std::future_status::ready);
}

void expectAuthOk(FramedClient& client, std::uint32_t seq) {
    const auto auth = client.readFrame();
    assert(auth.msgId == 4);
    assert(auth.seq == seq);
    assert(auth.payload == "auth-ok");
}

void expectLogicReply(FramedClient& client, std::string_view session, std::string_view payload) {
    const auto reply = client.readFrame();
    assert(reply.msgId == 4);
    assert(reply.payload == "logic:" + std::string(session) + ":" + std::string(payload));
}

}  // namespace

int main() {
    const auto port = allocateTestPort();

    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();

    mini::net::transport::TransportManager transportManager(baseLoop);
    mini::game::SessionManager sessionManager(baseLoop);
    mini::game::logic::LogicLoop logicLoop(
        {.fixedStep = std::chrono::milliseconds(4), .maxCommandsPerTick = 32});

    mini::net::TcpServerOptions options;
    options.numThreads = 2;
    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop,
        mini::net::InetAddress(port, true),
        "game-server-handoff-contract",
        options);
    auto* serverRaw = server.get();

    std::mutex processorMutex;
    std::set<std::thread::id> processorThreads;
    std::vector<std::string> processedCommands;
    logicLoop.setProcessor([&](const mini::game::logic::GameCommand& command,
                               std::vector<mini::game::logic::GameCommand>& outputs) {
        {
            std::scoped_lock lock(processorMutex);
            processorThreads.insert(std::this_thread::get_id());
            processedCommands.push_back(command.sessionId + ":" + command.payload);
        }

        mini::net::framing::PacketFramer framer;
        outputs.emplace_back(command.sessionId,
                             command.transportSessionId,
                             command.sourceTransport,
                             framer.encode(4, 0, 0, "logic:" + command.sessionId + ":" + command.payload));
    });

    mini::game::GameServerPipeline pipeline(*serverRaw, transportManager, sessionManager, logicLoop);
    std::promise<void> firstSubmitPromise;
    auto firstSubmitFuture = firstSubmitPromise.get_future();
    std::promise<void> secondSubmitPromise;
    auto secondSubmitFuture = secondSubmitPromise.get_future();
    std::once_flag firstSubmitOnce;
    std::once_flag secondSubmitOnce;
    pipeline.setMetricCallback([&](const mini::game::GamePipelineMetricSample& sample) {
        if (sample.event != mini::game::GamePipelineMetricEvent::LogicSubmitResult ||
            !sample.logicSubmitted) {
            return;
        }
        if (sample.sessionToken == "player-a") {
            std::call_once(firstSubmitOnce, [&] { firstSubmitPromise.set_value(); });
        } else if (sample.sessionToken == "player-b") {
            std::call_once(secondSubmitOnce, [&] { secondSubmitPromise.set_value(); });
        }
    });
    pipeline.install();

    auto started = std::make_shared<std::promise<void>>();
    auto startedFuture = started->get_future();
    baseLoop->queueInLoop([serverRaw, started] {
        serverRaw->start();
        started->set_value();
    });
    assert(startedFuture.wait_for(2s) == std::future_status::ready);
    logicLoop.start();

    FramedClient first(port);
    FramedClient second(port);
    first.sendFrame(1, 1, "player-a");
    second.sendFrame(1, 2, "player-b");
    expectAuthOk(first, 1);
    expectAuthOk(second, 2);
    assert(sessionManager.getTransportEndpoint("player-a"));
    assert(sessionManager.getTransportEndpoint("player-b"));

    first.sendFrame(2, 10, "move-left");
    second.sendFrame(2, 20, "move-right");
    expectLogicReply(first, "player-a", "move-left");
    expectLogicReply(second, "player-b", "move-right");
    assert(firstSubmitFuture.wait_for(2s) == std::future_status::ready);
    assert(secondSubmitFuture.wait_for(2s) == std::future_status::ready);

    first.close();
    waitForBaseLoopBarrier(baseLoop);
    std::this_thread::sleep_for(50ms);

    second.sendFrame(2, 21, "after-peer-close");
    expectLogicReply(second, "player-b", "after-peer-close");

    second.sendFrame(3, 22, "room-ping");
    const auto broadcast = second.readFrame();
    assert(broadcast.msgId == 4);
    assert(broadcast.seq == 22);
    assert(broadcast.payload == "broadcast:room-ping");

    {
        std::scoped_lock lock(processorMutex);
        assert(processorThreads.size() == 1);
        assert(processedCommands.size() == 3);
    }

    logicLoop.stop();

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
