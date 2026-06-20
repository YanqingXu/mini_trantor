#include "mini/game/SessionManager.h"
#include "mini/game/logic/LogicLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/transport/TransportEndpoint.h"
#include "mini/net/transport/TransportManager.h"

#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::pair<int, int> makeSocketPair() {
    int sockets[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    return {sockets[0], sockets[1]};
}

template <typename Fn>
auto runOnLoop(mini::net::EventLoop* loop, Fn&& fn) {
    using Result = decltype(fn());
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    loop->queueInLoop([promise, fn = std::forward<Fn>(fn)]() mutable {
        if constexpr (std::is_void_v<Result>) {
            fn();
            promise->set_value();
        } else {
            promise->set_value(fn());
        }
    });
    assert(future.wait_for(2s) == std::future_status::ready);
    if constexpr (!std::is_void_v<Result>) {
        return future.get();
    } else {
        future.get();
    }
}

std::string recvExact(int fd, std::size_t size) {
    timeval timeout{};
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    assert(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

    std::string out;
    out.reserve(size);
    while (out.size() < size) {
        char buffer[64]{};
        const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
        assert(n > 0);
        out.append(buffer, static_cast<std::size_t>(n));
    }
    return out;
}

}  // namespace

int main() {
    mini::net::EventLoopThread transportThread;
    auto* transportLoop = transportThread.startLoop();

    mini::net::transport::TransportManager transportManager(transportLoop);
    mini::game::SessionManager sessionManager(transportLoop);

    auto sockets = makeSocketPair();
    mini::net::TcpConnectionPtr connection;
    auto endpoint = runOnLoop(transportLoop, [transportLoop, fd = sockets.first, &connection] {
        connection = std::make_shared<mini::net::TcpConnection>(
            transportLoop,
            "transport-adapter-loopback",
            fd,
            mini::net::InetAddress(),
            mini::net::InetAddress());
        connection->connectEstablished();
        return mini::net::transport::TransportEndpoint::create(connection);
    });

    const auto transportSessionId = transportManager.registerEndpoint(endpoint);
    assert(transportSessionId != mini::net::transport::kInvalidTransportSessionId);
    runOnLoop(transportLoop, [&] {
        assert(transportManager.hasEndpoint(transportSessionId));
        auto session = sessionManager.ensureSession("player-loopback", transportSessionId);
        assert(session);
        assert(sessionManager.bindTransportEndpoint("player-loopback", endpoint));
        assert(sessionManager.getTransportEndpoint("player-loopback") == endpoint);
        assert(sessionManager.getTransportEndpoint(transportSessionId) == endpoint);
    });

    mini::game::logic::LogicLoop logicLoop(
        {.fixedStep = std::chrono::milliseconds(4), .maxCommandsPerTick = 8});
    logicLoop.setProcessor(
        [transportSessionId](const mini::game::logic::GameCommand& command,
                             std::vector<mini::game::logic::GameCommand>& outputs) {
            assert(command.sessionId == "player-loopback");
            assert(command.transportSessionId == transportSessionId);
            auto endpoint = command.sourceTransport.lock();
            assert(endpoint);
            assert(endpoint->sessionId() == transportSessionId);
            outputs.emplace_back(command.sessionId,
                                 command.transportSessionId,
                                 command.sourceTransport,
                                 "logic:" + command.payload);
        });
    logicLoop.start();

    assert(logicLoop.submit("player-loopback", transportSessionId, endpoint, "ping"));
    assert(recvExact(sockets.second, std::string("logic:ping").size()) == "logic:ping");

    logicLoop.stop();
    runOnLoop(transportLoop, [&] {
        transportManager.deregisterEndpoint(transportSessionId);
        if (connection) {
            connection->connectDestroyed();
            connection.reset();
        }
    });
    ::close(sockets.second);

    transportLoop->quit();
    return 0;
}
