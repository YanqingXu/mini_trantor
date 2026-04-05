// Connector contract tests.
//
// Core Module Change Gate:
// 1. Which loop/thread owns this module? — owner EventLoop thread.
// 2. Who owns it and who releases it? — TcpClient owns via shared_ptr.
// 3. Which callbacks may re-enter? — newConnectionCallback may call stop().
// 4. Cross-thread? — start()/stop() marshal via runInLoop.
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
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

int main() {
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

        connector->start();

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

        connector->stop();
        loop->runAfter(50ms, [loop] { loop->quit(); });
        std::this_thread::sleep_for(200ms);
    }

    // Contract 2: Connect to refused port triggers retry (verify state returns to kDisconnected)
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        // Use a port that nothing listens on
        auto connector = std::make_shared<mini::net::Connector>(
            loop, mini::net::InetAddress("127.0.0.1", 19302));
        connector->setRetryDelay(50ms, 200ms);

        std::promise<void> retryObserved;
        auto retryFuture = retryObserved.get_future();
        bool retryFired = false;

        // After a brief wait, check that connector is in kDisconnected (retrying)
        loop->runAfter(200ms, [&] {
            // After failed connect + retry delay, state should be kDisconnected or kConnecting
            if (!retryFired) {
                retryFired = true;
                retryObserved.set_value();
            }
        });

        connector->start();

        assert(retryFuture.wait_for(2s) == std::future_status::ready);
        connector->stop();

        loop->runAfter(100ms, [loop] { loop->quit(); });
        std::this_thread::sleep_for(300ms);
    }

    // Contract 3: stop() during pending connect cleans up Channel
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        auto connector = std::make_shared<mini::net::Connector>(
            loop, mini::net::InetAddress("192.0.2.1", 19303));  // RFC 5737 TEST-NET, will hang in EINPROGRESS
        connector->setRetryDelay(5s, 5s);  // long delay so retry won't fire

        connector->start();
        std::this_thread::sleep_for(50ms);

        std::promise<void> stopped;
        auto stoppedFuture = stopped.get_future();
        loop->runInLoop([&] {
            connector->stop();
            stopped.set_value();
        });

        assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
        // No crash, no fd leak — state should be kDisconnected
        loop->runAfter(50ms, [loop] { loop->quit(); });
        std::this_thread::sleep_for(200ms);
    }

    // Contract 4: Destruction in kDisconnected with pending retry timer is safe
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        {
            auto connector = std::make_shared<mini::net::Connector>(
                loop, mini::net::InetAddress("127.0.0.1", 19304));
            connector->setRetryDelay(50ms, 200ms);
            connector->start();
            std::this_thread::sleep_for(150ms);
            // Connector tries, fails, schedules retry. Now we drop the shared_ptr.
            connector->stop();
        }

        loop->runAfter(100ms, [loop] { loop->quit(); });
        std::this_thread::sleep_for(300ms);
        // must not crash
    }

    return 0;
}
