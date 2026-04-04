#include "mini/coroutine/Task.h"
#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/TcpConnection.h"

#include <array>
#include <cassert>
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
    testCallbackPathStillWorks();
    testReadAwaiterCrossThreadArmResumesOnOwnerLoopAndOnClose();
    testReadAwaiterSuppressesMessageCallback();
    testWriteAndWaitClosedResumeOnOwnerLoop();
    testSecondWriteWaiterIsRejected();
    testSecondReadWaiterIsRejected();
    return 0;
}
