#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/TcpConnection.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <future>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

int main() {
    int sockets[2];
    const int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
    assert(rc == 0);

    mini::net::EventLoop loop;
    auto connection =
        std::make_shared<mini::net::TcpConnection>(&loop, "socketpair#1", sockets[0], mini::net::InetAddress(), mini::net::InetAddress());

    int connectionEvents = 0;
    bool closeCalled = false;
    std::string received;
    std::promise<void> closed;
    auto closedFuture = closed.get_future();

    connection->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
        ++connectionEvents;
        if (!conn->connected()) {
            assert(conn->disconnected());
        }
    });
    connection->setMessageCallback([&](const mini::net::TcpConnectionPtr&, mini::net::Buffer* buffer) {
        received += buffer->retrieveAllAsString();
    });
    connection->setCloseCallback([&](const mini::net::TcpConnectionPtr&) {
        closeCalled = true;
        closed.set_value();
        loop.quit();
    });

    loop.runInLoop([&] { connection->connectEstablished(); });

    std::thread peer([fd = sockets[1]] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const char payload[] = "ping";
        const ssize_t written = ::write(fd, payload, sizeof(payload) - 1);
        assert(written == static_cast<ssize_t>(sizeof(payload) - 1));
        ::shutdown(fd, SHUT_WR);
        ::close(fd);
    });

    loop.loop();
    peer.join();

    assert(closeCalled);
    assert(received == "ping");
    assert(connectionEvents >= 2);
    assert(closedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);

    loop.runInLoop([&] { connection->connectDestroyed(); });
    connection.reset();

    return 0;
}
