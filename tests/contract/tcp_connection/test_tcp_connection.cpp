#include "mini/coroutine/Task.h"
#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/TcpConnection.h"

#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <future>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

using mini::net::TcpConnectionPtr;

std::array<int, 2> makeSocketPair() {
    std::array<int, 2> sockets{};
    const int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data());
    assert(rc == 0);
    return sockets;
}

void destroyConnectionOnLoop(mini::net::EventLoop& loop, TcpConnectionPtr& connection) {
    std::promise<void> destroyed;
    auto destroyedFuture = destroyed.get_future();

    loop.runInLoop([connection, &destroyed]() mutable {
        connection->connectDestroyed();
        connection.reset();
        destroyed.set_value();
    });

    connection.reset();
    assert(destroyedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
}

mini::coroutine::Task<void> readUntilResume(
    TcpConnectionPtr connection,
    std::promise<std::string>* message,
    std::promise<std::thread::id>* resumedOn) {
    std::string payload = co_await connection->asyncReadSome();
    message->set_value(std::move(payload));
    resumedOn->set_value(std::this_thread::get_id());
}

mini::coroutine::Task<void> writeThenWaitClosed(
    TcpConnectionPtr connection,
    std::string payload,
    std::promise<std::thread::id>* writeResumedOn,
    std::promise<std::thread::id>* closeResumedOn) {
    co_await connection->asyncWrite(std::move(payload));
    writeResumedOn->set_value(std::this_thread::get_id());
    co_await connection->waitClosed();
    closeResumedOn->set_value(std::this_thread::get_id());
}

mini::coroutine::Task<void> writeThenShutdown(
    TcpConnectionPtr connection,
    std::string payload,
    bool* resumed) {
    co_await connection->asyncWrite(std::move(payload));
    *resumed = true;
    connection->shutdown();
}

mini::coroutine::Task<void> suspendedRead(TcpConnectionPtr connection, bool* resumed) {
    std::string payload = co_await connection->asyncReadSome();
    assert(payload.empty());
    *resumed = true;
}

void testBackpressurePolicyRejectsInvalidThresholds() {
    auto sockets = makeSocketPair();

    mini::net::EventLoop loop;
    auto connection = std::make_shared<mini::net::TcpConnection>(
        &loop,
        "socketpair#backpressure-invalid",
        sockets[0],
        mini::net::InetAddress(),
        mini::net::InetAddress());

    bool rejectedLowWithoutHigh = false;
    try {
        connection->setBackpressurePolicy(0, 1);
    } catch (const std::invalid_argument& error) {
        rejectedLowWithoutHigh = true;
        assert(std::string_view(error.what()) == "backpressure low water mark requires a non-zero high water mark");
    }
    assert(rejectedLowWithoutHigh);

    bool rejectedNonHysteresisPair = false;
    try {
        connection->setBackpressurePolicy(1024, 1024);
    } catch (const std::invalid_argument& error) {
        rejectedNonHysteresisPair = true;
        assert(std::string_view(error.what()) == "backpressure low water mark must be smaller than high water mark");
    }
    assert(rejectedNonHysteresisPair);

    connection.reset();
    ::close(sockets[1]);
}

void testBackpressurePolicyPausesAndResumesReading() {
    auto sockets = makeSocketPair();

    const int senderFlags = ::fcntl(sockets[0], F_GETFL, 0);
    assert(senderFlags >= 0);
    assert(::fcntl(sockets[0], F_SETFL, senderFlags | O_NONBLOCK) == 0);

    const int peerFlags = ::fcntl(sockets[1], F_GETFL, 0);
    assert(peerFlags >= 0);
    assert(::fcntl(sockets[1], F_SETFL, peerFlags | O_NONBLOCK) == 0);

    const int sendBufferSize = 4096;
    const int rc = ::setsockopt(
        sockets[0],
        SOL_SOCKET,
        SO_SNDBUF,
        &sendBufferSize,
        static_cast<socklen_t>(sizeof(sendBufferSize)));
    assert(rc == 0);

    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* loop = loopThread.startLoop();
    auto connection = std::make_shared<mini::net::TcpConnection>(
        loop,
        "socketpair#backpressure-policy",
        sockets[0],
        mini::net::InetAddress(),
        mini::net::InetAddress());

    std::promise<void> firstHandled;
    std::promise<void> secondHandled;
    std::promise<void> closed;
    auto firstHandledFuture = firstHandled.get_future();
    auto secondHandledFuture = secondHandled.get_future();
    auto closedFuture = closed.get_future();

    connection->setBackpressurePolicy(1024, 512);
    connection->setMessageCallback(
        [&](const TcpConnectionPtr& conn, mini::net::Buffer* buffer) {
            const std::string message = buffer->retrieveAllAsString();
            if (message == "first") {
                conn->send(std::string(1 << 20, 'x'));
                firstHandled.set_value();
                return;
            }
            if (message == "second") {
                secondHandled.set_value();
                conn->forceClose();
            }
        });
    connection->setCloseCallback([loop, connection, &closed](const TcpConnectionPtr&) {
        closed.set_value();
        loop->queueInLoop([connection] { connection->connectDestroyed(); });
        loop->quit();
    });

    loop->queueInLoop([connection] { connection->connectEstablished(); });

    constexpr std::string_view firstPayload = "first";
    const ssize_t firstWritten = ::write(sockets[1], firstPayload.data(), firstPayload.size());
    assert(firstWritten == static_cast<ssize_t>(firstPayload.size()));
    assert(firstHandledFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);

    constexpr std::string_view secondPayload = "second";
    const ssize_t secondWritten = ::write(sockets[1], secondPayload.data(), secondPayload.size());
    assert(secondWritten == static_cast<ssize_t>(secondPayload.size()));
    assert(secondHandledFuture.wait_for(std::chrono::milliseconds(150)) == std::future_status::timeout);

    char buffer[8192];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::size_t drainedBytes = 0;
    while (secondHandledFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::read(sockets[1], buffer, sizeof(buffer));
        if (n > 0) {
            drainedBytes += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        break;
    }

    assert(drainedBytes > 0);
    assert(secondHandledFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(closedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);

    connection.reset();
    ::close(sockets[1]);
}

void testHighWaterMarkCallbackRunsOnOwnerLoop() {
    auto sockets = makeSocketPair();

    const int flags = ::fcntl(sockets[0], F_GETFL, 0);
    assert(flags >= 0);
    assert(::fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) == 0);

    const int sendBufferSize = 4096;
    const int rc = ::setsockopt(
        sockets[0],
        SOL_SOCKET,
        SO_SNDBUF,
        &sendBufferSize,
        static_cast<socklen_t>(sizeof(sendBufferSize)));
    assert(rc == 0);

    mini::net::EventLoopThread loopThread;
    mini::net::EventLoop* loop = loopThread.startLoop();
    auto connection = std::make_shared<mini::net::TcpConnection>(
        loop,
        "socketpair#high-water",
        sockets[0],
        mini::net::InetAddress(),
        mini::net::InetAddress());

    std::promise<std::pair<std::thread::id, std::size_t>> highWater;
    auto highWaterFuture = highWater.get_future();
    std::promise<void> closed;
    auto closedFuture = closed.get_future();

    connection->setHighWaterMarkCallback([&](const TcpConnectionPtr& conn, std::size_t bytes) {
        assert(conn->getLoop()->isInLoopThread());
        highWater.set_value({std::this_thread::get_id(), bytes});
        conn->forceClose();
    }, 1024);
    connection->setCloseCallback([loop, connection, &closed](const TcpConnectionPtr&) {
        closed.set_value();
        loop->queueInLoop([connection] { connection->connectDestroyed(); });
        loop->quit();
    });

    const auto callerThread = std::this_thread::get_id();
    loop->queueInLoop([connection] {
        connection->connectEstablished();
        std::string payload(1 << 20, 'x');
        connection->send(payload);
    });

    assert(highWaterFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    const auto [firedThread, bytes] = highWaterFuture.get();
    assert(firedThread != callerThread);
    assert(bytes >= 1024);
    assert(closedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);

    connection.reset();
    ::close(sockets[1]);
}

void testForceCloseMarshalsBackToOwnerLoop() {
    auto sockets = makeSocketPair();

    mini::net::EventLoop loop;
    auto connection = std::make_shared<mini::net::TcpConnection>(
        &loop,
        "socketpair#force-close",
        sockets[0],
        mini::net::InetAddress(),
        mini::net::InetAddress());

    int connectionEvents = 0;
    bool closeCalled = false;
    std::promise<void> closed;
    auto closedFuture = closed.get_future();

    connection->setConnectionCallback([&](const TcpConnectionPtr& conn) {
        ++connectionEvents;
        if (!conn->connected()) {
            assert(conn->disconnected());
        }
    });
    connection->setCloseCallback([&](const TcpConnectionPtr&) {
        closeCalled = true;
        closed.set_value();
        loop.quit();
    });

    loop.runInLoop([&] { connection->connectEstablished(); });

    std::thread closer([connection] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        connection->forceClose();
    });

    loop.loop();
    closer.join();

    assert(closeCalled);
    assert(connectionEvents >= 2);
    assert(closedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);

    destroyConnectionOnLoop(loop, connection);
    ::close(sockets[1]);
}

void testCallbackPathStillWorks() {
    auto sockets = makeSocketPair();

    mini::net::EventLoop loop;
    auto connection = std::make_shared<mini::net::TcpConnection>(
        &loop,
        "socketpair#callback",
        sockets[0],
        mini::net::InetAddress(),
        mini::net::InetAddress());

    int connectionEvents = 0;
    bool closeCalled = false;
    std::string received;
    std::promise<void> closed;
    auto closedFuture = closed.get_future();

    connection->setConnectionCallback([&](const TcpConnectionPtr& conn) {
        ++connectionEvents;
        if (!conn->connected()) {
            assert(conn->disconnected());
        }
    });
    connection->setMessageCallback([&](const TcpConnectionPtr&, mini::net::Buffer* buffer) {
        received += buffer->retrieveAllAsString();
    });
    connection->setCloseCallback([&](const TcpConnectionPtr&) {
        closeCalled = true;
        closed.set_value();
        loop.quit();
    });

    loop.runInLoop([&] { connection->connectEstablished(); });

    std::thread peer([fd = sockets[1]] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        constexpr std::string_view payload = "ping";
        const ssize_t written = ::write(fd, payload.data(), payload.size());
        assert(written == static_cast<ssize_t>(payload.size()));
        ::shutdown(fd, SHUT_WR);
        ::close(fd);
    });

    loop.loop();
    peer.join();

    assert(closeCalled);
    assert(received == "ping");
    assert(connectionEvents >= 2);
    assert(closedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);

    destroyConnectionOnLoop(loop, connection);
}

void testReadAwaiterCrossThreadArmResumesOnOwnerLoopAndOnClose() {
    auto sockets = makeSocketPair();

    mini::net::EventLoop loop;
    auto connection = std::make_shared<mini::net::TcpConnection>(
        &loop,
        "socketpair#read",
        sockets[0],
        mini::net::InetAddress(),
        mini::net::InetAddress());
    const auto ownerThread = std::this_thread::get_id();

    std::promise<std::string> messagePromise;
    std::promise<std::thread::id> resumedOnPromise;
    auto messageFuture = messagePromise.get_future();
    auto resumedOnFuture = resumedOnPromise.get_future();

    connection->setCloseCallback([&](const TcpConnectionPtr&) { loop.quit(); });
    loop.runInLoop([&] { connection->connectEstablished(); });

    std::thread starter([connection, &messagePromise, &resumedOnPromise] {
        readUntilResume(connection, &messagePromise, &resumedOnPromise).detach();
    });

    std::thread peer([fd = sockets[1]] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ::shutdown(fd, SHUT_WR);
        ::close(fd);
    });

    loop.loop();
    starter.join();
    peer.join();

    assert(messageFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    assert(resumedOnFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    assert(messageFuture.get().empty());
    assert(resumedOnFuture.get() == ownerThread);

    destroyConnectionOnLoop(loop, connection);
}

void testReadAwaiterSuppressesMessageCallback() {
    auto sockets = makeSocketPair();

    mini::net::EventLoop loop;
    auto connection = std::make_shared<mini::net::TcpConnection>(
        &loop,
        "socketpair#read-dispatch",
        sockets[0],
        mini::net::InetAddress(),
        mini::net::InetAddress());

    int messageCallbackCount = 0;
    std::promise<std::string> messagePromise;
    std::promise<std::thread::id> resumedOnPromise;
    auto messageFuture = messagePromise.get_future();
    auto resumedOnFuture = resumedOnPromise.get_future();

    connection->setMessageCallback([&](const TcpConnectionPtr&, mini::net::Buffer* buffer) {
        ++messageCallbackCount;
        buffer->retrieveAllAsString();
    });
    connection->setCloseCallback([&](const TcpConnectionPtr&) { loop.quit(); });
    loop.runInLoop([&] { connection->connectEstablished(); });

    std::thread starter([connection, &messagePromise, &resumedOnPromise] {
        readUntilResume(connection, &messagePromise, &resumedOnPromise).detach();
    });

    std::thread peer([fd = sockets[1]] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        constexpr std::string_view payload = "awaited";
        const ssize_t written = ::write(fd, payload.data(), payload.size());
        assert(written == static_cast<ssize_t>(payload.size()));
        ::shutdown(fd, SHUT_WR);
        ::close(fd);
    });

    loop.loop();
    starter.join();
    peer.join();

    assert(messageFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    assert(resumedOnFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    assert(messageFuture.get() == "awaited");
    assert(messageCallbackCount == 0);

    destroyConnectionOnLoop(loop, connection);
}

void testWriteAndWaitClosedResumeOnOwnerLoop() {
    auto sockets = makeSocketPair();

    mini::net::EventLoop loop;
    auto connection = std::make_shared<mini::net::TcpConnection>(
        &loop,
        "socketpair#write",
        sockets[0],
        mini::net::InetAddress(),
        mini::net::InetAddress());
    const auto ownerThread = std::this_thread::get_id();

    std::promise<std::thread::id> writeResumedOnPromise;
    std::promise<std::thread::id> closeResumedOnPromise;
    std::promise<std::string> peerReceivedPromise;
    auto writeResumedOnFuture = writeResumedOnPromise.get_future();
    auto closeResumedOnFuture = closeResumedOnPromise.get_future();
    auto peerReceivedFuture = peerReceivedPromise.get_future();

    connection->setCloseCallback([&](const TcpConnectionPtr&) { loop.quit(); });
    loop.runInLoop([&] { connection->connectEstablished(); });

    std::thread starter([connection, &writeResumedOnPromise, &closeResumedOnPromise] {
        writeThenWaitClosed(connection, "pong", &writeResumedOnPromise, &closeResumedOnPromise).detach();
    });

    std::thread peer([fd = sockets[1], &peerReceivedPromise] {
        char buffer[32] = {};
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        assert(n == 4);
        peerReceivedPromise.set_value(std::string(buffer, static_cast<std::size_t>(n)));
        ::shutdown(fd, SHUT_WR);
        ::close(fd);
    });

    loop.loop();
    starter.join();
    peer.join();

    assert(peerReceivedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    assert(writeResumedOnFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    assert(closeResumedOnFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    assert(peerReceivedFuture.get() == "pong");
    assert(writeResumedOnFuture.get() == ownerThread);
    assert(closeResumedOnFuture.get() == ownerThread);

    destroyConnectionOnLoop(loop, connection);
}

void testSecondWriteWaiterIsRejected() {
    auto sockets = makeSocketPair();
    auto* previousSigpipe = std::signal(SIGPIPE, SIG_IGN);
    const int flags = ::fcntl(sockets[0], F_GETFL, 0);
    assert(flags >= 0);
    const int setResult = ::fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK);
    assert(setResult == 0);

    mini::net::EventLoop loop;
    auto connection = std::make_shared<mini::net::TcpConnection>(
        &loop,
        "socketpair#double-write",
        sockets[0],
        mini::net::InetAddress(),
        mini::net::InetAddress());

    bool firstResumed = false;
    int closeCallbackCount = 0;
    std::promise<void> closed;
    auto closedFuture = closed.get_future();

    connection->setCloseCallback([&](const TcpConnectionPtr&) {
        ++closeCallbackCount;
        if (closeCallbackCount == 1) {
            closed.set_value();
        }
        loop.quit();
    });
    loop.runInLoop([&] { connection->connectEstablished(); });

    std::string largePayload(1 << 20, 'x');
    auto first = writeThenShutdown(connection, largePayload, &firstResumed);
    first.start();

    auto second = writeThenShutdown(connection, "short", &firstResumed);
    second.start();
    assert(second.done());

    bool rejected = false;
    try {
        second.result();
    } catch (const std::logic_error& error) {
        rejected = true;
        assert(std::string_view(error.what()) == "only one write waiter is allowed per TcpConnection");
    }
    assert(rejected);

    std::thread peer([fd = sockets[1]] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ::close(fd);
    });

    loop.loop();
    peer.join();

    assert(firstResumed);
    first.result();
    assert(closeCallbackCount == 1);
    assert(closedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);

    destroyConnectionOnLoop(loop, connection);
    std::signal(SIGPIPE, previousSigpipe);
}

void testSecondReadWaiterIsRejected() {
    auto sockets = makeSocketPair();

    mini::net::EventLoop loop;
    auto connection = std::make_shared<mini::net::TcpConnection>(
        &loop,
        "socketpair#double-read",
        sockets[0],
        mini::net::InetAddress(),
        mini::net::InetAddress());

    bool firstResumed = false;
    std::promise<void> closed;
    auto closedFuture = closed.get_future();

    connection->setCloseCallback([&](const TcpConnectionPtr&) {
        closed.set_value();
        loop.quit();
    });
    loop.runInLoop([&] { connection->connectEstablished(); });

    auto first = suspendedRead(connection, &firstResumed);
    first.start();

    auto second = suspendedRead(connection, &firstResumed);
    second.start();
    assert(second.done());

    bool rejected = false;
    try {
        second.result();
    } catch (const std::logic_error& error) {
        rejected = true;
        assert(std::string_view(error.what()) == "only one read waiter is allowed per TcpConnection");
    }
    assert(rejected);

    std::thread peer([fd = sockets[1]] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ::shutdown(fd, SHUT_WR);
        ::close(fd);
    });

    loop.loop();
    peer.join();

    assert(firstResumed);
    first.result();
    assert(closedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);

    destroyConnectionOnLoop(loop, connection);
}

}  // namespace

int main() {
    testBackpressurePolicyRejectsInvalidThresholds();
    testCallbackPathStillWorks();
    testBackpressurePolicyPausesAndResumesReading();
    testForceCloseMarshalsBackToOwnerLoop();
    testHighWaterMarkCallbackRunsOnOwnerLoop();
    testReadAwaiterCrossThreadArmResumesOnOwnerLoopAndOnClose();
    testReadAwaiterSuppressesMessageCallback();
    testWriteAndWaitClosedResumeOnOwnerLoop();
    testSecondWriteWaiterIsRejected();
    testSecondReadWaiterIsRejected();
    return 0;
}
