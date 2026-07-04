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
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
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
        assert(::send(fd_,
                      frame.data(),
                      frame.size(),
                      MSG_NOSIGNAL) == static_cast<ssize_t>(frame.size()));
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

class SecurityServer {
public:
    explicit SecurityServer(mini::game::GameServerPipeline::Options options)
        : port_(allocateTestPort()),
          baseLoop_(baseLoopThread_.startLoop()),
          transportManager_(baseLoop_),
          sessionManager_(baseLoop_),
          logicLoop_({.fixedStep = 20ms, .maxCommandsPerTick = 4}) {
        mini::net::TcpServerOptions serverOptions;
        serverOptions.numThreads = 1;
        server_ = std::make_unique<mini::net::TcpServer>(
            baseLoop_,
            mini::net::InetAddress(port_, true),
            "game-gateway-security-contract",
            serverOptions);
        serverRaw_ = server_.get();
        pipeline_ = std::make_unique<mini::game::GameServerPipeline>(
            *serverRaw_,
            transportManager_,
            sessionManager_,
            logicLoop_,
            std::move(options));
    }

    ~SecurityServer() {
        if (server_) {
            stop();
        }
    }

    uint16_t port() const noexcept {
        return port_;
    }

    mini::game::SessionManager& sessionManager() noexcept {
        return sessionManager_;
    }

    void start() {
        pipeline_->install();
        auto started = std::make_shared<std::promise<void>>();
        auto startedFuture = started->get_future();
        baseLoop_->queueInLoop([serverRaw = serverRaw_, started] {
            serverRaw->start();
            started->set_value();
        });
        assert(startedFuture.wait_for(2s) == std::future_status::ready);
    }

    void stop() {
        auto stopped = std::make_shared<std::promise<void>>();
        auto stoppedFuture = stopped->get_future();
        baseLoop_->queueInLoop([serverRaw = serverRaw_, stopped] {
            serverRaw->stop();
            stopped->set_value();
        });
        assert(stoppedFuture.wait_for(2s) == std::future_status::ready);

        pipeline_.reset();
        auto serverOwner = std::make_shared<std::unique_ptr<mini::net::TcpServer>>(std::move(server_));
        auto destroyed = std::make_shared<std::promise<void>>();
        auto destroyedFuture = destroyed->get_future();
        baseLoop_->queueInLoop([serverOwner, baseLoop = baseLoop_, destroyed] {
            serverOwner->reset();
            baseLoop->quit();
            destroyed->set_value();
        });
        assert(destroyedFuture.wait_for(2s) == std::future_status::ready);
        serverRaw_ = nullptr;
    }

private:
    uint16_t port_{0};
    mini::net::EventLoopThread baseLoopThread_;
    mini::net::EventLoop* baseLoop_{nullptr};
    mini::net::transport::TransportManager transportManager_;
    mini::game::SessionManager sessionManager_;
    mini::game::logic::LogicLoop logicLoop_;
    std::unique_ptr<mini::net::TcpServer> server_;
    mini::net::TcpServer* serverRaw_{nullptr};
    std::unique_ptr<mini::game::GameServerPipeline> pipeline_;
};

void expectAuthOk(FramedClient& client, std::uint32_t seq) {
    const auto frame = client.readFrame();
    assert(frame.msgId == 4);
    assert(frame.seq == seq);
    assert(frame.payload == "auth-ok");
}

void testDefaultSecurityKeepsLegacyTokenPayloadUntouched() {
    mini::game::GameServerPipeline::Options options;
    SecurityServer server(options);
    server.start();

    FramedClient client(server.port());
    client.sendFrame(1, 1, "legacy|token");
    expectAuthOk(client, 1);

    assert(server.sessionManager().getSession("legacy|token") != nullptr);
    assert(server.sessionManager().getSession("legacy") == nullptr);
}

void testFreshNonceDoesNotBlockStickyReconnect() {
    mini::game::GameServerPipeline::Options options;
    options.security.authReplayWindow = 5s;
    options.security.authTokenNonceDelimiter = "|";

    SecurityServer server(options);
    server.start();

    {
        FramedClient first(server.port());
        first.sendFrame(1, 1, "secure-sticky|nonce-a");
        expectAuthOk(first, 1);
    }

    auto firstSession = server.sessionManager().getSession("secure-sticky");
    assert(firstSession != nullptr);

    FramedClient second(server.port());
    second.sendFrame(1, 2, "secure-sticky|nonce-b");
    expectAuthOk(second, 2);

    auto secondSession = server.sessionManager().getSession("secure-sticky");
    assert(secondSession == firstSession);
}

}  // namespace

int main() {
    testDefaultSecurityKeepsLegacyTokenPayloadUntouched();
    testFreshNonceDoesNotBlockStickyReconnect();
    return 0;
}
