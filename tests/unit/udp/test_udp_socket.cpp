#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/udp/UdpSocket.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstring>
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
    mini::net::EventLoop loop;

    mini::net::udp::UdpSocket socket(&loop,
                                     mini::net::InetAddress(serverPort, true),
                                     true,
                                     "unit-udp-socket");

    std::promise<std::string> packetPromise;
    auto packetFuture = packetPromise.get_future();

    std::promise<std::string> responsePromise;
    auto responseFuture = responsePromise.get_future();

    socket.setPacketCallback([&](std::string_view packet, const mini::net::InetAddress& peer) {
        packetPromise.set_value(std::string(packet));
        socket.sendTo("pong", peer);
        loop.quit();
    });

    socket.start();

    std::thread client([serverPort, responsePromise = std::move(responsePromise)]() mutable {
        const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
        assert(fd >= 0);

        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(serverPort);
        assert(::inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr) == 1);

        const std::string payload = "ping";
        const ssize_t sent = ::sendto(
            fd,
            payload.data(),
            payload.size(),
            0,
            reinterpret_cast<const sockaddr*>(&serverAddr),
            sizeof(serverAddr));
        assert(sent == static_cast<ssize_t>(payload.size()));

        timeval timeout{};
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        assert(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

        char response[16]{};
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

    loop.loop();
    client.join();

    assert(packetFuture.get() == "ping");
    assert(responseFuture.get() == "pong");

    return 0;
}
