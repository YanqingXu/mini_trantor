// v6-alpha Task-06 — Broadcast payload 共享与复用契约测试
// 目标：同一 PayloadPtr 可用于多次广播命令，且不会发生内容串扰。

#include "mini/net/broadcast/BroadcastDispatcher.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/buffer/PayloadPool.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"

#include <array>
#include <cassert>
#include <chrono>
#include <future>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::pair<int, int> makeSocketPair() {
    std::array<int, 2> sockets{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) == 0);
    return {sockets[0], sockets[1]};
}

std::string readExactly(int fd, std::size_t expectedLen) {
    std::string out;
    out.reserve(expectedLen);
    while (out.size() < expectedLen) {
        char buf[128];
        const auto n = ::recv(fd, buf, sizeof(buf), 0);
        assert(n > 0);
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

}  // namespace

int main() {
    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();
    mini::net::EventLoopThread ioLoopThread;
    auto* ioLoop = ioLoopThread.startLoop();

    const auto sockets = makeSocketPair();

    std::promise<mini::net::TcpConnectionPtr> connReady;
    auto connReadyFuture = connReady.get_future();
    auto closed = std::make_shared<std::promise<void>>();
    auto closedFuture = closed->get_future();

    ioLoop->queueInLoop([&connReady, fd = sockets.first, ioLoop, closed] {
        auto conn = std::make_shared<mini::net::TcpConnection>(
            ioLoop, "payload-sharing", fd, mini::net::InetAddress(), mini::net::InetAddress());
        conn->setCloseCallback([closed](const mini::net::TcpConnectionPtr&) {
            closed->set_value();
        });
        conn->connectEstablished();
        connReady.set_value(conn);
    });

    auto connection = connReadyFuture.get();
    auto dispatcher = std::make_shared<mini::net::broadcast::BroadcastDispatcher>(baseLoop);
    auto payloadPool = std::make_shared<mini::net::buffer::PayloadPool>(baseLoop);

    auto payload = payloadPool->acquire(std::string_view{"shared-payload."});
    auto payloadAlias = payload;

    std::vector<mini::net::broadcast::BroadcastRouter::LoopBatch> firstBatch{
        {ioLoop, {connection}},
    };
    std::vector<mini::net::broadcast::BroadcastRouter::LoopBatch> secondBatch{
        {ioLoop, {connection}},
    };

    dispatcher->dispatch(std::move(firstBatch), std::move(payload));
    dispatcher->dispatch(std::move(secondBatch), std::move(payloadAlias));

    const std::string expect = "shared-payload.shared-payload.";
    const auto received = readExactly(sockets.second, expect.size());
    assert(received == expect);
    const auto recycleDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    bool recycled = false;
    while (std::chrono::steady_clock::now() < recycleDeadline) {
        if (payloadPool->inUseCount() == 0 && payloadPool->cachedCount() >= 1) {
            recycled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(recycled);

    ioLoop->queueInLoop([connection] { connection->forceClose(); });
    assert(closedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    auto recycle = std::make_shared<std::promise<void>>();
    auto recycleFuture = recycle->get_future();
    ioLoop->queueInLoop([connection = std::move(connection), recycle] mutable {
        connection->connectDestroyed();
        connection.reset();
        recycle->set_value();
    });
    assert(recycleFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    ioLoop->quit();
    baseLoop->quit();
    return 0;
}
