#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/transport/TransportTypes.h"
#include "mini/net/udp/UdpServer.h"

#include <arpa/inet.h>
#include <cassert>
#include <future>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

uint16_t allocatePort() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    assert(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);

    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

}  // namespace

int main() {
    const uint16_t serverPort = allocatePort();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();
    mini::net::udp::UdpServer server(loop, mini::net::InetAddress(serverPort, true), "integration-udp-server");

    std::promise<mini::net::transport::TransportSessionId> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();
    std::promise<void> startPromise;
    auto startFuture = startPromise.get_future();

    server.setMessageCallback([&sessionPromise](mini::net::transport::TransportSessionId sessionId,
                                                std::string_view packet,
                                                const mini::net::InetAddress&) {
        if (packet == "join") {
            sessionPromise.set_value(sessionId);
        }
    });

    loop->queueInLoop([&]() {
        server.start();
        startPromise.set_value();
    });
    startFuture.get();

    std::promise<std::string> responsePromise;
    auto responseFuture = responsePromise.get_future();

    std::thread client([serverPort, responsePromise = std::move(responsePromise)]() mutable {
        const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
        assert(fd >= 0);

        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(serverPort);
        assert(::inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr) == 1);

        const std::string payload = "join";
        assert(::sendto(
                   fd,
                   payload.data(),
                   payload.size(),
                   0,
                   reinterpret_cast<const sockaddr*>(&serverAddr),
                   sizeof(serverAddr)) == static_cast<ssize_t>(payload.size()));

        timeval timeout{};
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        assert(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

        char response[32]{};
        sockaddr_storage from{};
        socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
        const ssize_t recvLen = ::recvfrom(fd,
                                          response,
                                          sizeof(response),
                                          0,
                                          reinterpret_cast<sockaddr*>(&from),
                                          &fromLen);

        if (recvLen > 0) {
            responsePromise.set_value(std::string(response, static_cast<std::size_t>(recvLen)));
        } else {
            responsePromise.set_value("");
        }
        ::close(fd);
    });

    const auto sessionId = sessionFuture.get();
    assert(sessionId >= mini::net::transport::kFirstTransportSessionId);

    server.sendTo(sessionId, "cross-thread-ack");

    const auto response = responseFuture.get();
    assert(response == "cross-thread-ack");

    auto closedPromise = std::make_shared<std::promise<void>>();
    auto closedFuture = closedPromise->get_future();
    loop->queueInLoop([&server, sessionId, closedPromise]() {
        server.closeSession(sessionId);
        closedPromise->set_value();
    });
    closedFuture.get();
    assert(server.sessionCount() == 0);

    auto stopPromise = std::make_shared<std::promise<void>>();
    auto stopFuture = stopPromise->get_future();
    loop->queueInLoop([&server, stopPromise]() {
        server.stop();
        stopPromise->set_value();
    });
    stopFuture.get();

    client.join();
    loop->runInLoop([loop]() { loop->quit(); });

    return 0;
}
