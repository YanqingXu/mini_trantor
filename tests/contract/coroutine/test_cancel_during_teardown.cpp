// Intent: async_timer / tcp_connection / loop_handle.
// A token notification already extracted by cancel() must not own the old loop/connection.
#include "mini/coroutine/SleepAwaitable.h"
#include "mini/coroutine/Task.h"
#include "mini/net/EventLoop.h"
#include "mini/net/SocketsOps.h"
#include "mini/net/TcpConnection.h"

#include <array>
#include <barrier>
#include <cassert>
#include <chrono>
#include <memory>
#include <string_view>
#include <thread>
#ifndef _WIN32
#include <sys/socket.h>
#endif

using namespace mini::coroutine;
using namespace mini::net;
using namespace std::chrono_literals;

namespace {
Task<void> sleeping(EventLoop* loop, CancellationToken token, int* destroyed) {
    struct Probe { int* count; ~Probe() { ++*count; } } probe{destroyed};
    co_await asyncSleep(loop, 1h, std::move(token));
    assert(false); // no loop iteration is run in these owner teardown cases
}

Task<void> reading(TcpConnectionPtr connection, CancellationToken token, int* destroyed) {
    struct Probe { int* count; ~Probe() { ++*count; } } probe{destroyed};
    co_await connection->asyncReadSome(1, std::move(token));
    assert(false);
}

void sleepCancellationMayFinishAfterLoopDestruction() {
    int destroyed = 0;
    constexpr int attempts = 200;
    for (int i = 0; i < attempts; ++i) {
        CancellationSource source;
        std::barrier race(2);
        std::thread cancel;
        {
            EventLoop loop;
            auto task = sleeping(&loop, source.token(), &destroyed);
            task.start();
            cancel = std::thread([&] { race.arrive_and_wait(); source.cancel(); });
            race.arrive_and_wait();
        }
        cancel.join();
    }
    assert(destroyed == attempts);
}

void tcpCancellationCannotRetainTheConnectionOffOwner() {
    int destroyed = 0;
    constexpr int attempts = 200;
    for (int i = 0; i < attempts; ++i) {
        CancellationSource source;
        std::barrier race(2);
        std::thread cancel;
        std::weak_ptr<TcpConnection> observed;
        {
            EventLoop loop;
            std::array<SocketFd, 2> pair;
#ifdef _WIN32
            sockets::createSocketPairOrDie(pair.data());
#else
            assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair.data()) == 0);
#endif
            auto connection = std::make_shared<TcpConnection>(
                &loop, "cancel-teardown", pair[0], InetAddress(), InetAddress());
            observed = connection;
            connection->connectEstablished();
            {
                auto task = reading(connection, source.token(), &destroyed);
                task.start();
                cancel = std::thread([&] { race.arrive_and_wait(); source.cancel(); });
                race.arrive_and_wait();
            }
            connection->forceClose();
            connection->connectDestroyed();
            connection.reset();
            sockets::close(pair[1]);
        }
        cancel.join();
        assert(observed.expired());
    }
    assert(destroyed == attempts);
}
} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "all";
    if (mode == "sleep" || mode == "all") { sleepCancellationMayFinishAfterLoopDestruction(); }
    if (mode == "tcp" || mode == "all") { tcpCancellationCannotRetainTheConnectionOffOwner(); }
}
