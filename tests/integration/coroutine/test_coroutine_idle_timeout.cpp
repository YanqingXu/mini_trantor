// Integration test: coroutine echo handler with per-connection idle timeout.
//
// Uses runAfter + cancel pattern for idle timeout alongside asyncReadSome.
// If no data arrives within the timeout, the connection is force-closed.

#include "mini/coroutine/SleepAwaitable.h"
#include "mini/coroutine/Task.h"
#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <future>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

uint16_t allocateTestPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    assert(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

// Coroutine echo session with idle timeout using runAfter + cancel pattern.
mini::coroutine::Task<void> echoSessionWithTimeout(
    mini::net::TcpConnectionPtr connection,
    std::chrono::steady_clock::duration idleTimeout,
    std::promise<std::string>* closeReason) {
    mini::net::EventLoop* loop = connection->getLoop();

    while (connection->connected()) {
        // Set idle timeout: if no data arrives, force close.
        auto timer = loop->runAfter(idleTimeout, [connection] {
            if (connection->connected()) {
                connection->forceClose();
            }
        });

        std::string message = co_await connection->asyncReadSome();

        // Cancel the idle timer (data arrived or connection closed).
        loop->cancel(timer);

        if (message.empty()) {
            closeReason->set_value("eof_or_close");
            co_return;
        }

        co_await connection->asyncWrite(std::move(message));
    }

    closeReason->set_value("disconnected");
}

}  // namespace

int main() {
    // Integration 1: Normal echo works within timeout
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::net::TcpServer server(&loop, mini::net::InetAddress(port, true), "timeout_echo");

        std::promise<std::string> closeReason;
        auto closeReasonFuture = closeReason.get_future();

        server.setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (conn->connected()) {
                echoSessionWithTimeout(conn, 500ms, &closeReason).detach();
            } else {
                loop.quit();
            }
        });
        server.start();

        std::thread client([port] {
            const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
            assert(fd >= 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            assert(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

            // Send data within timeout
            const std::string payload = "hello timeout";
            assert(::write(fd, payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));

            char buf[64] = {};
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            assert(n == static_cast<ssize_t>(payload.size()));
            assert(std::string(buf, static_cast<std::size_t>(n)) == payload);

            ::close(fd);
        });

        loop.loop();
        client.join();

        assert(closeReasonFuture.wait_for(1s) == std::future_status::ready);
        assert(closeReasonFuture.get() == "eof_or_close");
    }

    // Integration 2: Connection is closed after idle timeout (no data sent)
    {
        const uint16_t port = allocateTestPort();
        mini::net::EventLoop loop;
        mini::net::TcpServer server(&loop, mini::net::InetAddress(port, true), "timeout_echo2");

        std::promise<std::string> closeReason;
        auto closeReasonFuture = closeReason.get_future();

        server.setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (conn->connected()) {
                echoSessionWithTimeout(conn, 300ms, &closeReason).detach();
            } else {
                loop.quit();
            }
        });
        server.start();

        std::thread client([port] {
            const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
            assert(fd >= 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            assert(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

            // Don't send anything — wait for server to close due to timeout
            char buf[64] = {};
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            // Server force-closed: read returns 0 (EOF) or error
            assert(n <= 0);

            ::close(fd);
        });

        loop.loop();
        client.join();

        assert(closeReasonFuture.wait_for(1s) == std::future_status::ready);
        // The session ended due to empty read (force close triggers EOF on async read)
    }

    return 0;
}
