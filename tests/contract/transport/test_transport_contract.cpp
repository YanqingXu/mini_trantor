#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/kcp/KcpSession.h"
#include "mini/net/kcp/KcpTransport.h"
#include "mini/net/transport/TransportEndpoint.h"
#include "mini/net/transport/TransportManager.h"
#include "mini/net/udp/UdpServer.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <type_traits>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

std::pair<int, int> makeSocketPair() {
    int sockets[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    return {sockets[0], sockets[1]};
}

uint16_t allocateUdpPort() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const auto port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
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

void testTcpEndpointThroughManager(mini::net::EventLoop* loop,
                                   mini::net::transport::TransportManager& manager) {
    auto sockets = makeSocketPair();
    mini::net::TcpConnectionPtr connection;
    auto endpoint = runOnLoop(loop, [loop, fd = sockets.first, &connection] {
        connection = std::make_shared<mini::net::TcpConnection>(
            loop, "transport-contract-tcp", fd, mini::net::InetAddress(), mini::net::InetAddress());
        connection->connectEstablished();
        return mini::net::transport::TransportEndpoint::create(connection);
    });

    const auto id = manager.registerEndpoint(endpoint);
    assert(id != mini::net::transport::kInvalidTransportSessionId);
    runOnLoop(loop, [&manager, id] {
        assert(manager.hasEndpoint(id));
    });

    std::thread sender([&] { manager.send(id, "tcp-ok"); });
    sender.join();

    char buffer[16]{};
    const auto n = ::recv(sockets.second, buffer, sizeof(buffer), 0);
    assert(n == 6);
    assert(std::string(buffer, buffer + n) == "tcp-ok");

    manager.close(id);
    manager.deregisterEndpoint(id);
    runOnLoop(loop, [&manager, id] {
        assert(!manager.hasEndpoint(id));
    });
    runOnLoop(loop, [&connection] {
        if (connection) {
            connection->connectDestroyed();
            connection.reset();
        }
    });
    ::close(sockets.second);
}

void testUdpEndpointThroughManager(mini::net::EventLoop* loop,
                                   mini::net::transport::TransportManager& manager) {
    const auto port = allocateUdpPort();
    mini::net::udp::UdpServer server(loop, mini::net::InetAddress(port, true), "transport-contract-udp");

    std::promise<mini::net::transport::TransportSessionId> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();
    server.setMessageCallback([&](mini::net::transport::TransportSessionId sessionId,
                                  std::string_view packet,
                                  const mini::net::InetAddress&) {
        if (packet == "join") {
            sessionPromise.set_value(sessionId);
        }
    });
    runOnLoop(loop, [&] { server.start(); });

    const int clientFd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    assert(clientFd >= 0);
    timeval timeout{};
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    assert(::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr) == 1);
    const std::string join = "join";
    assert(::sendto(clientFd,
                    join.data(),
                    join.size(),
                    0,
                    reinterpret_cast<const sockaddr*>(&serverAddr),
                    sizeof(serverAddr)) == static_cast<ssize_t>(join.size()));

    assert(sessionFuture.wait_for(2s) == std::future_status::ready);
    const auto sessionId = sessionFuture.get();
    auto endpoint = runOnLoop(loop, [&server, sessionId] {
        return server.getTransportEndpoint(sessionId);
    });
    assert(endpoint);
    assert(endpoint->transportKind() == mini::net::transport::TransportKind::kUdp);

    const auto id = manager.registerEndpoint(endpoint);
    assert(id == sessionId);
    runOnLoop(loop, [&manager, id] { assert(manager.hasEndpoint(id)); });
    manager.send(id, "udp-ok");

    char buffer[32]{};
    sockaddr_storage from{};
    socklen_t fromLen = sizeof(from);
    const auto n = ::recvfrom(clientFd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
    assert(n == 6);
    assert(std::string(buffer, buffer + n) == "udp-ok");

    manager.close(id);
    manager.deregisterEndpoint(id);
    runOnLoop(loop, [&] { server.stop(); });
    ::close(clientFd);
}

void testKcpEndpointThroughManager(mini::net::EventLoop* loop,
                                   mini::net::transport::TransportManager& manager) {
    const auto port = allocateUdpPort();
    mini::net::kcp::KcpTransport transport(
        loop,
        mini::net::InetAddress(port, true),
        "transport-contract-kcp");

    auto session = runOnLoop(loop, [&] {
        transport.start();
        return transport.openSession(mini::net::InetAddress("127.0.0.1", allocateUdpPort()));
    });
    assert(session);
    assert(session->transportKind() == mini::net::transport::TransportKind::kKcp);

    const auto id = manager.registerEndpoint(session);
    assert(id == session->sessionId());
    runOnLoop(loop, [&manager, id] { assert(manager.hasEndpoint(id)); });

    std::thread closer([&] { manager.close(id); });
    closer.join();
    runOnLoop(loop, [&manager, &transport, id, session] {
        assert(!session->connected());
        manager.deregisterEndpoint(id);
        transport.stop();
    });
}

}  // namespace

int main() {
    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();
    mini::net::transport::TransportManager manager(loop);

    testTcpEndpointThroughManager(loop, manager);
    testUdpEndpointThroughManager(loop, manager);
    testKcpEndpointThroughManager(loop, manager);

    loop->quit();
    return 0;
}
