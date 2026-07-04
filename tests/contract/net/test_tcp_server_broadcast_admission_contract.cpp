#include "mini/net/Buffer.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"
#include "mini/net/TcpServerOptions.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
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
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    void sendBytes(std::string_view bytes) {
        assert(::send(fd_, bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size()));
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
    const std::string payload = "blocked-broadcast";

    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();

    mini::net::TcpServerOptions options;
    options.numThreads = 1;
    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop,
        mini::net::InetAddress(port, true),
        "broadcast-admission-contract",
        options);
    auto* serverRaw = server.get();

    std::promise<void> admittedPromise;
    auto admittedFuture = admittedPromise.get_future();
    std::once_flag admittedOnce;
    serverRaw->setBroadcastAdmissionCallback(
        [&](const mini::net::BroadcastMetricSample& sample) {
            assert(sample.event == mini::net::BroadcastMetricEvent::Routed);
            assert(sample.loop == baseLoop);
            assert(sample.targeted);
            assert(sample.requestedSessions == 1);
            assert(sample.fanoutConnections == 1);
            assert(sample.payloadBytes == payload.size());
            assert(sample.routeLatency >= mini::net::BroadcastMetricSample::Duration::zero());
            std::call_once(admittedOnce, [&] { admittedPromise.set_value(); });
            return false;
        });

    serverRaw->setMessageCallback([&](const mini::net::TcpConnectionPtr& connection,
                                      mini::net::Buffer* buffer) {
        const auto token = buffer->retrieveAllAsString();
        assert(!token.empty());
        serverRaw->bindBroadcastSession(connection, token);
        serverRaw->joinBroadcastGroup(token, "room");
        connection->send("ok");
    });

    auto started = std::make_shared<std::promise<void>>();
    auto startedFuture = started->get_future();
    baseLoop->queueInLoop([serverRaw, started] {
        serverRaw->start();
        started->set_value();
    });
    assert(startedFuture.wait_for(2s) == std::future_status::ready);

    RawClient client(port);
    client.sendBytes("player");
    assert(client.readExact(2) == "ok");
    waitBaseLoop(baseLoop);

    serverRaw->broadcastGroup("room", payload);
    assert(admittedFuture.wait_for(2s) == std::future_status::ready);
    client.expectNoData(100ms);

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
