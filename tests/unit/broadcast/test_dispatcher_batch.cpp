#include "mini/net/broadcast/BroadcastDispatcher.h"
#include "mini/net/Callbacks.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/buffer/Payload.h"
#include "mini/net/TcpConnection.h"

#include <array>
#include <cassert>
#include <chrono>
#include <future>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::pair<int, int> makeSocketPair() {
    std::array<int, 2> fds{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds.data()) == 0);
    return {fds[0], fds[1]};
}

std::string readExactly(int fd, std::size_t expectedLen) {
    std::string out;
    out.reserve(expectedLen);
    timeval timeout{};
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    assert(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

    while (out.size() < expectedLen) {
        char buf[64];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

}  // namespace

int main() {
    mini::net::EventLoop baseLoop;
    mini::net::EventLoop ioLoop;

    std::thread ioLoopThread([&ioLoop] { ioLoop.loop(); });

    const auto sockets = makeSocketPair();

    std::promise<mini::net::TcpConnectionPtr> connReady;
    mini::net::TcpConnectionPtr conn;
    ioLoop.queueInLoop([&conn, fd = sockets.first, &connReady, &ioLoop] {
        conn = std::make_shared<mini::net::TcpConnection>(
            &ioLoop, "dispatch-session", fd,
            mini::net::InetAddress(), mini::net::InetAddress());
        conn->connectEstablished();
        connReady.set_value(conn);
    });

    auto connection = connReady.get_future().get();
    auto dispatcher = std::make_shared<mini::net::broadcast::BroadcastDispatcher>(&baseLoop);

    using mini::net::broadcast::BroadcastRouter;
    std::vector<BroadcastRouter::LoopBatch> batches;
    batches.push_back({&ioLoop, {connection}});

    dispatcher->dispatch(std::move(batches),
                        std::make_shared<mini::net::buffer::Payload>("alpha-"));
    dispatcher->dispatch(
        {BroadcastRouter::LoopBatch{&ioLoop, {connection}}},
        std::make_shared<mini::net::buffer::Payload>("beta"));

    const std::string expect = "alpha-beta";
    const auto received = readExactly(sockets.second, expect.size());
    assert(received == expect);

    std::promise<void> closed;
    auto closedFuture = closed.get_future();
    connection->setCloseCallback([&closed](const mini::net::TcpConnectionPtr&) {
        closed.set_value();
    });
    ioLoop.queueInLoop([connection] { connection->forceClose(); });

    assert(closedFuture.wait_for(std::chrono::seconds{2}) == std::future_status::ready);

    ioLoop.quit();
    ioLoopThread.join();
    ::close(sockets.second);
    return 0;
}
