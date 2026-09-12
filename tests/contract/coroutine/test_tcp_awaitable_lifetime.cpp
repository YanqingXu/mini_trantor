// Intent: tcp_connection / connection_awaiter_registry; unregister before frame destruction.
#include "mini/coroutine/Task.h"
#include "mini/net/EventLoop.h"
#include "mini/net/SocketsOps.h"
#include "mini/net/TcpConnection.h"

#include <array>
#include <cassert>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#ifndef _WIN32
#include <sys/socket.h>
#endif

using namespace std::chrono_literals;
using mini::coroutine::CancellationSource;
using mini::coroutine::CancellationToken;
using mini::coroutine::Task;
using namespace mini::net;

namespace {
struct Fixture {
    EventLoop loop;
    SocketFd peer{};
    TcpConnectionPtr connection;

    Fixture() {
        std::array<SocketFd, 2> pair;
#ifdef _WIN32
        sockets::createSocketPairOrDie(pair.data());
#else
        const auto rc = ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair.data());
        assert(rc == 0);
#endif
        peer = pair[1];
        const int capacity = 4096;
        const auto sendOption = ::setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF,
            reinterpret_cast<const char*>(&capacity), sizeof(capacity));
        assert(sendOption == 0);
        connection = std::make_shared<TcpConnection>(&loop, "lifetime", pair[0],
                                                     InetAddress(), InetAddress());
        connection->connectEstablished();
    }

    ~Fixture() {
        if (connection) {
            connection->forceClose();
            connection->connectDestroyed();
            connection.reset();
        }
        sockets::close(peer);
    }

    void drainClosed() {
        connection->forceClose();
        connection->connectDestroyed();
        loop.quit();
        loop.loop(); // process already-queued completion after the frame was destroyed
        connection.reset();
    }
};

enum class Operation { Read, WriteQueued, WritePending, Close };

Task<void> wait(TcpConnectionPtr connection, Operation operation, int& resumed, int& destroyed,
                CancellationToken token = {}) {
    struct Probe { int& count; ~Probe() { ++count; } } probe{destroyed};
    switch (operation) {
    case Operation::Read: co_await connection->asyncReadSome(1, std::move(token)); break;
    case Operation::WriteQueued: co_await connection->asyncWrite("x", std::move(token)); break;
    case Operation::WritePending:
        co_await connection->asyncWrite(std::string(4 * 1024 * 1024, 'x'), std::move(token)); break;
    case Operation::Close: co_await connection->waitClosed(std::move(token)); break;
    }
    ++resumed;
}

void destroyWaiter(Operation operation, bool queueCompletion) {
    Fixture fixture;
    int resumed = 0, destroyed = 0;
    {
        auto task = wait(fixture.connection, operation, resumed, destroyed);
        task.start();
        assert(!task.done());
        if (queueCompletion) { fixture.connection->forceClose(); }
    }
    assert(destroyed == 1);
    fixture.drainClosed();
    assert(resumed == 0);
    assert(destroyed == 1);
}

void cancellationAlreadyQueued(Operation operation) {
    Fixture fixture;
    CancellationSource cancellation;
    int resumed = 0, destroyed = 0;
    {
        auto task = wait(fixture.connection, operation, resumed, destroyed, cancellation.token());
        task.start();
        cancellation.cancel();
    }
    fixture.drainClosed();
    assert(resumed == 0);
    assert(destroyed == 1);
}

Task<void> readAndQuit(TcpConnectionPtr connection, EventLoop* loop, bool* received) {
    auto result = co_await connection->asyncReadSome();
    *received = result && *result == "replacement";
    loop->quit();
}

void replacementReadAfterDestruction() {
    Fixture fixture;
    int resumed = 0, destroyed = 0;
    { auto task = wait(fixture.connection, Operation::Read, resumed, destroyed); task.start(); }
    bool received = false;
    auto replacement = readAndQuit(fixture.connection, &fixture.loop, &received);
    replacement.start();
    const auto written = sockets::write(fixture.peer, "replacement", 11);
    assert(written == 11);
    fixture.loop.runAfter(1s, [&] { fixture.loop.quit(); });
    fixture.loop.loop();
    assert(replacement.done());
    replacement.result(); // duplicate registration must not be left by the destroyed waiter
    assert(received);
}

void destroyBeforeQueuedArming(Operation operation) {
    Fixture fixture;
    int resumed = 0, destroyed = 0;
    auto task = wait(fixture.connection, operation, resumed, destroyed);
    std::thread starter([&] { task.start(); });
    starter.join(); // await_suspend has returned; owner has not processed arming
    task = {}; // owner-loop destruction invalidates the queued registration
    fixture.drainClosed();
    assert(resumed == 0);
    assert(destroyed == 1);
}

void duplicateFromAnotherThreadIsDeliveredToTask() {
    Fixture fixture;
    int resumed = 0, destroyed = 0;
    auto first = wait(fixture.connection, Operation::Read, resumed, destroyed);
    first.start();
    auto second = wait(fixture.connection, Operation::Read, resumed, destroyed);
    std::thread starter([&] { second.start(); });
    starter.join();
    fixture.loop.queueInLoop([&] { fixture.connection->forceClose(); fixture.loop.quit(); });
    fixture.loop.loop();
    assert(first.done() && second.done());
    first.result();
    bool rejected = false;
    try { second.result(); } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    assert(resumed == 1);
    assert(destroyed == 2);
}

void queuedCompletionStillReservesSlot() {
    Fixture fixture;
    int resumed = 0, destroyed = 0;
    auto first = wait(fixture.connection, Operation::WriteQueued, resumed, destroyed);
    first.start(); // socket accepted the byte, but await_resume has not run
    auto second = wait(fixture.connection, Operation::WriteQueued, resumed, destroyed);
    second.start();
    assert(second.done());
    bool rejected = false;
    try { second.result(); } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    fixture.loop.quit();
    fixture.loop.loop();
    assert(first.done());
    first.result();
    assert(resumed == 1);
    assert(destroyed == 2);
}

Task<void> writeWithInheritedCancellation(TcpConnectionPtr connection, bool* cancelled) {
    auto result = co_await connection->asyncWrite("must-not-send");
    *cancelled = !result && result.error() == NetError::Cancelled;
}

void preCancelledWriteSubmitsNoBytes() {
    Fixture fixture;
    CancellationSource source;
    source.cancel();
    bool cancelled = false;
    auto task = writeWithInheritedCancellation(fixture.connection, &cancelled);
    task.setCancellationToken(source.token());
    task.start();
    fixture.loop.quit();
    fixture.loop.loop();
    assert(task.done());
    task.result();
    assert(cancelled);
    char buffer[32];
    const auto received = sockets::read(fixture.peer, buffer, sizeof(buffer));
    assert(received < 0 && sockets::isWouldBlock(sockets::lastError()));
}
} // namespace

int main() {
    for (auto operation : {Operation::Read, Operation::WriteQueued, Operation::WritePending, Operation::Close}) {
        destroyWaiter(operation, false);
        destroyWaiter(operation, true);
        cancellationAlreadyQueued(operation);
        destroyBeforeQueuedArming(operation);
    }
    replacementReadAfterDestruction();
    duplicateFromAnotherThreadIsDeliveredToTask();
    queuedCompletionStillReservesSlot();
    preCancelledWriteSubmitsNoBytes();
}
