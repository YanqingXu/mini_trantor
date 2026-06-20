// test_kcp_reliable_flow.cpp
//
// 1) 有响应端点：验证会话的端到端收发。
// 2) 无响应端点：验证发送端可重传 tick 后触发会话超时关闭（丢包路径回归）。

#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/kcp/KcpTransport.h"
#include "mini/net/kcp/KcpSession.h"
#include "mini/net/transport/TransportTypes.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <future>
#include <string>
#include <string_view>
#include <thread>
#include <sys/socket.h>
#include <sys/time.h>
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

void test_kcp_reliable_exchange() {
    auto loopPort1 = allocatePort();
    auto loopPort2 = allocatePort();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransport transportA(
        loop,
        mini::net::InetAddress(loopPort1, true),
        "integration-kcp-a");
    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(loopPort2, true),
        "integration-kcp-b");

    std::promise<bool> recvOnB;
    auto recvOnBFuture = recvOnB.get_future();
    std::promise<bool> recvOnA;
    auto recvOnAFuture = recvOnA.get_future();

    std::atomic<int> pingCount{0};
    std::atomic<int> pongCount{0};

    std::promise<mini::net::transport::TransportSessionId> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();

    loop->queueInLoop([&] {
        transportA.setMessageCallback([&](mini::net::transport::TransportSessionId,
                                          std::string_view packet,
                                          const mini::net::InetAddress&) {
            if (packet.rfind("pong", 0) == 0 && ++pongCount == 2) {
                recvOnA.set_value(true);
            }
        });

        transportB.setMessageCallback([&](mini::net::transport::TransportSessionId sessionId,
                                          std::string_view packet,
                                          const mini::net::InetAddress&) {
            if (packet == "ping1" || packet == "ping2") {
                const auto currentPing = ++pingCount;
                transportB.sendTo(sessionId, std::string("pong") + std::to_string(currentPing));
                if (packet == "ping2") {
                    recvOnB.set_value(true);
                }
            }
        });

        transportA.start();
        transportB.start();

        auto session = transportA.openSession(mini::net::InetAddress("127.0.0.1", loopPort2));
        if (!session) {
            sessionPromise.set_value(mini::net::transport::kInvalidTransportSessionId);
            return;
        }

        session->send("ping1");
        session->send("ping2");
        sessionPromise.set_value(session->sessionId());
    });

    const auto sessionId = sessionFuture.get();
    assert(sessionId != mini::net::transport::kInvalidTransportSessionId);

    assert(recvOnBFuture.get());
    assert(recvOnAFuture.get());

    std::promise<void> closePromise;
    auto closeFuture = closePromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(sessionId);
        transportA.stop();
        transportB.stop();
        closePromise.set_value();
    });
    closeFuture.get();

    std::promise<std::size_t> remainPromise;
    auto remainFuture = remainPromise.get_future();
    loop->queueInLoop([&] {
        remainPromise.set_value(transportA.sessionCount() + transportB.sessionCount());
    });
    assert(remainFuture.get() == 0);
}

void test_kcp_retransmission_timeout() {
    auto loopPort1 = allocatePort();
    // 没有任何应用监听该端口，模拟持续丢包/无 ack 场景。
    auto unreachablePort = allocatePort();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransport sender(
        loop,
        mini::net::InetAddress(loopPort1, true),
        "integration-kcp-timeout-sender");

    sender.start();

    std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();

    loop->queueInLoop([&] {
        auto session = sender.openSession(mini::net::InetAddress("127.0.0.1", unreachablePort));
        if (session) {
            session->send("timeout-ping");
        }
        sessionPromise.set_value(session);
    });

    const auto session = sessionFuture.get();
    assert(session);

    // 不应立即关闭，说明发送进入了重试等待/重传 tick 通道。
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    assert(sender.sessionCount() == 1);

    // 重传窗口触发后应最终超时关闭会话。
    bool closedByTimeout = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2200);
    while (std::chrono::steady_clock::now() < deadline) {
        if (sender.sessionCount() == 0) {
            closedByTimeout = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    auto connectedPromise = std::make_shared<std::promise<bool>>();
    auto connectedFuture = connectedPromise->get_future();
    loop->queueInLoop([session, connectedPromise]() {
        connectedPromise->set_value(session->connected());
    });
    assert(!connectedFuture.get() || sender.sessionCount() == 0);

    assert(closedByTimeout);

    std::promise<void> closePromise;
    auto closeFuture = closePromise.get_future();
    loop->queueInLoop([&] {
        sender.stop();
        closePromise.set_value();
    });
    closeFuture.get();
}

void test_kcp_retransmission_retry_count_observed() {
    const auto loopPort = allocatePort();
    const auto dropPort = allocatePort();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransport sender(
        loop,
        mini::net::InetAddress(loopPort, true),
        "integration-kcp-timeout-retry-count");

    sender.start();

    std::atomic<std::size_t> wireRecvCount{0};
    std::atomic<bool> stopDropper{false};

    std::thread dropper([&] {
        const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
        assert(fd >= 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(dropPort);

        assert(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 250000;
        assert(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

        while (!stopDropper.load(std::memory_order_acquire)) {
            char buffer[1024]{};
            sockaddr_storage from{};
            socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
            const ssize_t n = ::recvfrom(fd,
                                         buffer,
                                         sizeof(buffer),
                                         0,
                                         reinterpret_cast<sockaddr*>(&from),
                                         &fromLen);
            if (n >= 0) {
                ++wireRecvCount;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            break;
        }
        ::close(fd);
    });

    std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();

    loop->queueInLoop([&] {
        auto session = sender.openSession(mini::net::InetAddress("127.0.0.1", dropPort));
        if (session) {
            session->send("retry-probe");
        }
        sessionPromise.set_value(session);
    });

    const auto session = sessionFuture.get();
    assert(session);

    std::promise<std::size_t> initialCountPromise;
    auto initialCountFuture = initialCountPromise.get_future();
    loop->queueInLoop([&] { initialCountPromise.set_value(sender.sessionCount()); });
    assert(initialCountFuture.get() == 1);

    const auto retransmissionObserveDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < retransmissionObserveDeadline) {
        if (wireRecvCount.load(std::memory_order_acquire) >= 2) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    assert(wireRecvCount.load(std::memory_order_acquire) >= 2);

    bool closedByTimeout = false;
    const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < closeDeadline) {
        if (sender.sessionCount() == 0) {
            closedByTimeout = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    stopDropper.store(true, std::memory_order_release);
    dropper.join();

    assert(closedByTimeout);
    assert(wireRecvCount.load(std::memory_order_acquire) >= 3);

    std::promise<void> closePromise;
    auto closeFuture = closePromise.get_future();
    loop->queueInLoop([&] {
        sender.stop();
        closePromise.set_value();
    });
    closeFuture.get();
}

}  // namespace

int main() {
    test_kcp_reliable_exchange();
    test_kcp_retransmission_timeout();
    test_kcp_retransmission_retry_count_observed();
    return 0;
}
