#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/udp/UdpSocket.h"

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
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

void sendDatagram(uint16_t serverPort, std::string_view payload) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    assert(fd >= 0);

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(serverPort);
    assert(::inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr) == 1);

    const ssize_t sent = ::sendto(
        fd,
        payload.data(),
        payload.size(),
        0,
        reinterpret_cast<const sockaddr*>(&serverAddr),
        sizeof(serverAddr));
    assert(sent == static_cast<ssize_t>(payload.size()));
    ::close(fd);
}

void sendBurst(uint16_t serverPort, int count) {
    for (int i = 0; i < count; ++i) {
        sendDatagram(serverPort, "burst");
    }
}

void testUdpSocketLoopback() {
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
}

void testUdpSocketReadBudgetStopsOneReadBatch() {
    using namespace std::chrono_literals;

    const uint16_t serverPort = allocatePort();
    mini::net::EventLoop loop;

    mini::net::udp::UdpSocket socket(&loop,
                                     mini::net::InetAddress(serverPort, true),
                                     true,
                                     "unit-udp-budget");
    socket.setMaxDatagramsPerRead(3);
    assert(socket.maxDatagramsPerRead() == 3);

    std::size_t received = 0;
    bool budgetMetricObserved = false;

    socket.setPacketCallback([&](std::string_view packet, const mini::net::InetAddress&) {
        assert(packet == "burst");
        ++received;
    });
    socket.setMetricCallback([&](const mini::net::UdpMetricSample& sample) {
        assert(sample.event == mini::net::UdpMetricEvent::ReadBatch);
        assert(sample.loop == &loop);
        assert(sample.socketName == "unit-udp-budget");
        assert(sample.maxDatagramsPerRead == 3);
        assert(sample.readDuration >= mini::net::UdpMetricSample::Duration::zero());
        if (!sample.budgetExhausted) {
            return;
        }
        assert(sample.datagramsRead == 3);
        assert(sample.bytesRead == 15);
        assert(received == 3);
        budgetMetricObserved = true;
        loop.quit();
    });

    socket.start();
    sendBurst(serverPort, 16);
    loop.runAfter(1s, [&loop] { loop.quit(); });

    loop.loop();

    assert(budgetMetricObserved);
    assert(received == 3);
}

void testUdpSocketReportsSynchronousPathMtuFailure() {
    const uint16_t serverPort = allocatePort();
    mini::net::EventLoop loop;

    mini::net::udp::UdpSocket socket(&loop,
                                     mini::net::InetAddress(0, true),
                                     true,
                                     "unit-udp-path-mtu-failure");

    const mini::net::InetAddress peer("127.0.0.1", serverPort);
    const std::string oversizedPayload(70000, 'x');
    bool observed = false;

    socket.setPathMtuFailureCallback(
        [&](const mini::net::udp::PathMtuFailure& failure) {
            observed = true;
            assert(failure.errorCode == EMSGSIZE);
            assert(failure.peerAddr.toIpPort() == peer.toIpPort());
            assert(failure.failedDatagramPayloadSize == oversizedPayload.size());
            assert(failure.suggestedDatagramPayloadSize == 0);
            assert(failure.source == mini::net::udp::PathMtuSignalSource::kLocalSend);
            assert(failure.quotedUdpPayloadPrefix.size() ==
                   mini::net::udp::kMaxPathMtuQuotedUdpPayloadPrefix);
            assert(failure.quotedUdpPayloadPrefix ==
                   oversizedPayload.substr(
                       0,
                       mini::net::udp::kMaxPathMtuQuotedUdpPayloadPrefix));
        });

    socket.sendTo(oversizedPayload, peer);

    assert(observed);
}

void testUdpSocketPlatformPathMtuSignalToggle() {
    mini::net::EventLoop loop;
    mini::net::udp::UdpSocket socket(&loop,
                                     mini::net::InetAddress(0, true),
                                     true,
                                     "unit-udp-path-mtu-error-queue");

#ifdef __linux__
    assert(socket.enablePlatformPathMtuSignals(true));
    assert(socket.platformPathMtuSignalsEnabled());
    assert(socket.enablePlatformPathMtuSignals(false));
    assert(!socket.platformPathMtuSignalsEnabled());
    assert(socket.enablePathMtuErrorQueue(true));
    assert(socket.pathMtuErrorQueueEnabled());
    assert(socket.platformPathMtuSignalsEnabled());
    assert(socket.enablePathMtuErrorQueue(false));
    assert(!socket.pathMtuErrorQueueEnabled());
    assert(!socket.platformPathMtuSignalsEnabled());
#else
    assert(!socket.enablePlatformPathMtuSignals(true));
    assert(!socket.platformPathMtuSignalsEnabled());
    assert(!socket.enablePathMtuErrorQueue(true));
    assert(!socket.pathMtuErrorQueueEnabled());
#endif
}

void testUdpSocketRawIcmpIpv6ToggleIsBestEffort() {
    mini::net::EventLoop loop;
    mini::net::udp::UdpSocket socket(&loop,
                                     mini::net::InetAddress("::1", 0, true),
                                     true,
                                     "unit-udp-raw-icmpv6");

    const bool enabled = socket.enableRawIcmpPathMtuListener(true);
    assert(socket.rawIcmpPathMtuListenerEnabled() == enabled);
    assert(socket.enableRawIcmpPathMtuListener(false));
    assert(!socket.rawIcmpPathMtuListenerEnabled());
}

}  // namespace

int main() {
    testUdpSocketLoopback();
    testUdpSocketReadBudgetStopsOneReadBatch();
    testUdpSocketReportsSynchronousPathMtuFailure();
    testUdpSocketPlatformPathMtuSignalToggle();
    testUdpSocketRawIcmpIpv6ToggleIsBestEffort();

    return 0;
}
