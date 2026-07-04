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

    bool waitForClose(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        char byte{};
        while (std::chrono::steady_clock::now() < deadline) {
            const auto n = ::recv(fd_, &byte, sizeof(byte), MSG_DONTWAIT);
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
            return false;
        }
        return false;
    }

private:
    int fd_{-1};
    mini::net::framing::PacketFramer framer_;
    std::string input_;
};

mini::game::GameServerPipeline::Options securityOptions() {
    mini::game::GameServerPipeline::Options options;
    options.security.maxAuthTokenBytes = 64;
    options.security.authReplayWindow = 5s;
    options.security.authTokenNonceDelimiter = "|";
    options.security.maxFramesPerSessionPerWindow = 2;
    options.security.sessionRateWindow = 1s;
    return options;
}

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
            "game-gateway-security",
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

    mini::game::GameServerPipeline& pipeline() noexcept {
        return *pipeline_;
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

void installValidator(mini::game::GameServerPipeline& pipeline) {
    pipeline.setAuthTokenValidator([](std::string_view sessionToken, std::string_view nonce) {
        return sessionToken.starts_with("secure-") && !nonce.empty();
    });
}

void testValidatorRejectsInvalidAuthToken() {
    SecurityServer server(securityOptions());
    installValidator(server.pipeline());

    std::promise<void> rejectedPromise;
    auto rejectedFuture = rejectedPromise.get_future();
    std::once_flag rejectedOnce;
    server.pipeline().setSecurityMetricCallback(
        [&](const mini::game::GameSecurityMetricSample& sample) {
            if (sample.event == mini::game::GameSecurityMetricEvent::AuthRejected &&
                sample.reason == mini::game::GameSecurityReason::AuthTokenValidatorRejected) {
                assert(sample.sessionToken == "bad-token");
                assert(sample.msgId == 1);
                assert(sample.payloadBytes > 0);
                std::call_once(rejectedOnce, [&] { rejectedPromise.set_value(); });
            }
        });
    server.start();

    FramedClient client(server.port());
    client.sendFrame(1, 1, "bad-token|nonce-1");
    assert(rejectedFuture.wait_for(2s) == std::future_status::ready);
    assert(client.waitForClose(2s));
}

void testAuthReplayIsRejectedWithinWindow() {
    SecurityServer server(securityOptions());
    installValidator(server.pipeline());

    std::promise<void> replayPromise;
    auto replayFuture = replayPromise.get_future();
    std::once_flag replayOnce;
    server.pipeline().setSecurityMetricCallback(
        [&](const mini::game::GameSecurityMetricSample& sample) {
            if (sample.event == mini::game::GameSecurityMetricEvent::AuthRejected &&
                sample.reason == mini::game::GameSecurityReason::AuthReplay) {
                assert(sample.sessionToken == "secure-replay");
                assert(sample.msgId == 1);
                assert(sample.payloadBytes > 0);
                std::call_once(replayOnce, [&] { replayPromise.set_value(); });
            }
        });
    server.start();

    {
        FramedClient first(server.port());
        first.sendFrame(1, 1, "secure-replay|nonce-1");
        const auto auth = first.readFrame();
        assert(auth.msgId == 4);
        assert(auth.seq == 1);
        assert(auth.payload == "auth-ok");
    }

    FramedClient replay(server.port());
    replay.sendFrame(1, 2, "secure-replay|nonce-1");
    assert(replayFuture.wait_for(2s) == std::future_status::ready);
    assert(replay.waitForClose(2s));
}

void testAuthenticatedSessionRateLimitClosesConnection() {
    SecurityServer server(securityOptions());
    installValidator(server.pipeline());

    std::promise<void> ratePromise;
    auto rateFuture = ratePromise.get_future();
    std::once_flag rateOnce;
    server.pipeline().setSecurityMetricCallback(
        [&](const mini::game::GameSecurityMetricSample& sample) {
            if (sample.event == mini::game::GameSecurityMetricEvent::RateLimited &&
                sample.reason == mini::game::GameSecurityReason::SessionRateLimit) {
                assert(sample.sessionToken == "secure-rate");
                assert(sample.msgId == 2);
                assert(sample.currentValue == 3);
                assert(sample.limit == 2);
                std::call_once(rateOnce, [&] { ratePromise.set_value(); });
            }
        });
    server.start();

    FramedClient client(server.port());
    client.sendFrame(1, 1, "secure-rate|nonce-rate");
    const auto auth = client.readFrame();
    assert(auth.msgId == 4);
    assert(auth.payload == "auth-ok");

    client.sendFrame(2, 2, "move-1");
    client.sendFrame(2, 3, "move-2");
    client.sendFrame(2, 4, "move-3");

    assert(rateFuture.wait_for(2s) == std::future_status::ready);
    assert(client.waitForClose(2s));
}

}  // namespace

int main() {
    testValidatorRejectsInvalidAuthToken();
    testAuthReplayIsRejectedWithinWindow();
    testAuthenticatedSessionRateLimitClosesConnection();
    return 0;
}
