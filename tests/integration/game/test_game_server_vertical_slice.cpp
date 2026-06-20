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
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
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
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    void sendFrame(std::uint32_t msgId, std::uint32_t seq, std::string_view payload) {
        const auto frame = framer_.encode(msgId, 0, seq, payload);
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

private:
    int fd_{-1};
    mini::net::framing::PacketFramer framer_;
    std::string input_;
};

}  // namespace

int main() {
    const auto port = allocateTestPort();

    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();

    mini::net::transport::TransportManager transportManager(baseLoop);
    mini::game::SessionManager sessionManager(baseLoop);
    mini::game::logic::LogicLoop logicLoop(
        {.fixedStep = std::chrono::milliseconds(4), .maxCommandsPerTick = 16});

    mini::net::TcpServerOptions options;
    options.numThreads = 1;
    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop,
        mini::net::InetAddress(port, true),
        "game-server-vertical-slice",
        options);
    auto* serverRaw = server.get();

    logicLoop.setProcessor([](const mini::game::logic::GameCommand& command,
                              std::vector<mini::game::logic::GameCommand>& outputs) {
        mini::net::framing::PacketFramer framer;
        outputs.emplace_back(command.sessionId,
                             command.transportSessionId,
                             command.sourceTransport,
                             framer.encode(4, 0, 0, "logic:" + command.payload));
    });

    mini::game::GameServerPipeline pipeline(*serverRaw, transportManager, sessionManager, logicLoop);
    pipeline.install();

    auto started = std::make_shared<std::promise<void>>();
    auto startedFuture = started->get_future();
    baseLoop->queueInLoop([serverRaw, started] {
        serverRaw->start();
        started->set_value();
    });
    assert(startedFuture.wait_for(2s) == std::future_status::ready);

    logicLoop.start();

    FramedClient client(port);
    client.sendFrame(1, 1, "player-vslice");
    auto auth = client.readFrame();
    assert(auth.msgId == 4);
    assert(auth.seq == 1);
    assert(auth.payload == "auth-ok");

    auto session = sessionManager.getSession("player-vslice");
    assert(session);
    assert(sessionManager.getTransportEndpoint("player-vslice"));

    client.sendFrame(2, 2, "move");
    auto response = client.readFrame();
    assert(response.msgId == 4);
    assert(response.payload == "logic:move");

    client.sendFrame(3, 3, "hello");
    auto broadcast = client.readFrame();
    assert(broadcast.msgId == 4);
    assert(broadcast.seq == 3);
    assert(broadcast.payload == "broadcast:hello");

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
