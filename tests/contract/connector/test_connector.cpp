// Connector contract tests.
//
// Core Module Change Gate:
// 1. Which loop/thread owns this module? — owner EventLoop thread.
// 2. Who owns it and who releases it? — TcpClient owns via shared_ptr.
// 3. Which callbacks may re-enter? — newConnectionCallback may call stop().
// 4. Cross-thread? — callers marshal start()/stop()/restart() via runInLoop.
// 5. Test file? — This file.

#include "mini/net/Connector.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/SocketsOps.h"
#include "mini/net/Acceptor.h"

#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

int main() {
    // Owner-only entry points reject before touching mutable connection state.
    {
        mini::net::EventLoop loop;
        auto connector = std::make_shared<mini::net::Connector>(
            &loop, mini::net::InetAddress("127.0.0.1", 1));
        std::thread caller([&] {
            for (auto operation : {&mini::net::Connector::start,
                                   &mini::net::Connector::stop,
                                   &mini::net::Connector::restart}) {
                bool rejected = false;
                try { ((*connector).*operation)(); }
                catch (const std::runtime_error&) { rejected = true; }
                assert(rejected);
            }
        });
        caller.join();
        assert(connector->state() == mini::net::Connector::kDisconnected);
    }
    // Contract 1: Successful connect delivers fd through callback on owner loop
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        // Set up a server to accept connections using a raw listen socket
        const uint16_t port = 19301;
        const int listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        assert(listenFd >= 0);
        int optval = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
        sockaddr_in saddr{};
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        saddr.sin_port = htons(port);
        assert(::bind(listenFd, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr)) == 0);
        assert(::listen(listenFd, 5) == 0);

        std::promise<int> connectedFd;
        auto connectedFdFuture = connectedFd.get_future();

        auto connector = std::make_shared<mini::net::Connector>(
            loop, mini::net::InetAddress("127.0.0.1", port));

        connector->setNewConnectionCallback([&](int sockfd) {
            assert(loop->isInLoopThread());
            connectedFd.set_value(sockfd);
        });

        loop->runInLoop([connector] { connector->start(); });

        auto status = connectedFdFuture.wait_for(2s);
        assert(status == std::future_status::ready);
        const int fd = connectedFdFuture.get();
        assert(fd >= 0);

        // Accept on listen side and close
        sockaddr_in peerAddr{};
        socklen_t peerLen = sizeof(peerAddr);
        const int acceptedFd = ::accept4(listenFd, reinterpret_cast<sockaddr*>(&peerAddr), &peerLen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (acceptedFd >= 0) {
            ::close(acceptedFd);
        }
        ::close(fd);
        ::close(listenFd);

        loop->queueInLoop([connector] { connector->stop(); });
        loopThread.stop();
    }

    // Contract 2: Connect to refused port triggers retry (verify state returns to kDisconnected)
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        // Use a port that nothing listens on
        mini::net::ConnectorOptions options;
        options.enableRetry = true;
        auto connector = std::make_shared<mini::net::Connector>(
            loop, mini::net::InetAddress("127.0.0.1", 19302), options);
        connector->setRetryDelay(50ms, 200ms);

        std::promise<void> retryObserved;
        auto retryFuture = retryObserved.get_future();
        bool retryFired = false;

        // Observe an actual retry transition, not elapsed wall-clock time.
        connector->setConnectorEventCallback([&](const auto&, mini::net::ConnectorEvent event) {
            if (event == mini::net::ConnectorEvent::RetryScheduled && !retryFired) {
                assert(connector->state() == mini::net::Connector::kDisconnected);
                retryFired = true;
                retryObserved.set_value();
            }
        });

        loop->runInLoop([connector] { connector->start(); });

        const auto retryStatus = retryFuture.wait_for(2s);
        assert(retryStatus == std::future_status::ready);
        loop->queueInLoop([connector] { connector->stop(); });
        loopThread.stop();
    }

    // Contract 3: stop() during pending connect cleans up Channel
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        mini::net::ConnectorOptions options;
        options.enableRetry = true;
        auto connector = std::make_shared<mini::net::Connector>(
            loop, mini::net::InetAddress("192.0.2.1", 19303), options); // route may fail immediately
        connector->setRetryDelay(5s, 5s);  // long delay so retry won't fire

        std::promise<void> stopped;
        auto stoppedFuture = stopped.get_future();
        loop->runInLoop([&] {
            connector->start();
            // Start and stop run in one pending-functor batch, before readiness
            // can interleave; start itself is now always queued.
            loop->queueInLoop([&] {
                connector->stop();
                assert(connector->state() == mini::net::Connector::kDisconnected);
                stopped.set_value();
            });
        });

        const auto stopStatus = stoppedFuture.wait_for(2s);
        assert(stopStatus == std::future_status::ready);
        loopThread.stop(); // also drains deferred Channel destruction
    }

    // Contract 4: Destruction in kDisconnected with pending retry timer is safe
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        mini::net::ConnectorOptions options;
        options.enableRetry = true;
        auto connector = std::make_shared<mini::net::Connector>(
            loop, mini::net::InetAddress("127.0.0.1", 19304), options);
        std::weak_ptr<mini::net::Connector> lifetime = connector;
        connector->setRetryDelay(5s, 5s);
        std::promise<void> retryScheduled;
        auto retryFuture = retryScheduled.get_future();
        connector->setConnectorEventCallback([&](const auto&, mini::net::ConnectorEvent event) {
            if (event == mini::net::ConnectorEvent::RetryScheduled) { retryScheduled.set_value(); }
        });
        loop->runInLoop([connector] { connector->start(); });
        const auto retryStatus = retryFuture.wait_for(2s);
        assert(retryStatus == std::future_status::ready);
        loop->queueInLoop([connector = std::move(connector)]() mutable {
            connector->stop();
            connector.reset();
        });
        loopThread.stop();
        assert(lifetime.expired());
    }

    return 0;
}
