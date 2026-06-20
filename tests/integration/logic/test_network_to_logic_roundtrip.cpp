#include "mini/game/logic/LogicLoop.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpServer.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <netinet/in.h>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

namespace {

uint16_t allocateTestPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    const int bound = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    assert(bound == 0);

    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    const int named = ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    assert(named == 0);

    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

void runLogicRoundTripClient(uint16_t port, const std::string& payload, std::string& replyOut) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const int converted = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    assert(converted == 1);

    assert(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(::write(fd, payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));

    char buffer[128]{};
    const ssize_t readn = ::read(fd, buffer, sizeof(buffer));
    assert(readn > 0);
    replyOut.assign(buffer, buffer + static_cast<std::size_t>(readn));
    ::close(fd);
}

}  // namespace

int main() {
    const auto port = allocateTestPort();
    mini::net::EventLoopThread loopThread;
    auto* baseLoop = loopThread.startLoop();

    auto server = std::make_shared<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "integration-logic-roundtrip");
    server->setThreadNum(1);

    mini::game::logic::LogicLoop logicLoop({.fixedStep = std::chrono::milliseconds(8), .maxCommandsPerTick = 4});
    logicLoop.setProcessor([](const mini::game::logic::GameCommand& command,
                              std::vector<mini::game::logic::GameCommand>& outputs) {
        outputs.push_back({command.sessionId, command.sourceConnection, "ok:" + command.payload});
    });

    server->setLogicMessageCallback([&logicLoop](const mini::net::TcpConnectionPtr& connection,
                                               std::string_view payload) {
        logicLoop.submit(std::string(payload), connection, std::string(payload));
    });

    auto serverStarted = std::make_shared<std::promise<void>>();
    auto serverStartedFuture = serverStarted->get_future();
    baseLoop->queueInLoop([server, serverStarted]() {
        server->start();
        serverStarted->set_value();
    });
    assert(serverStartedFuture.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);

    logicLoop.start();

    std::string reply;
    runLogicRoundTripClient(port, "ping", reply);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    assert(reply == "ok:ping");

    logicLoop.stop();
    auto serverForShutdown = std::move(server);

    auto stopped = std::make_shared<std::promise<void>>();
    auto stoppedFuture = stopped->get_future();
    baseLoop->queueInLoop([server = std::move(serverForShutdown), baseLoop, stopped]() mutable {
        server->stop();
        stopped->set_value();
        baseLoop->quit();
        server.reset();
    });
    assert(stoppedFuture.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready);

    return 0;
}
