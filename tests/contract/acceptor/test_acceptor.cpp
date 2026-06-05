#include "mini/net/Acceptor.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/SocketsOps.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <future>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

int connectTo(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return fd;
}

}  // namespace

int main() {
    // Contract 1: listen() registers readable interest; accepted fd is forwarded through callback
    {
        mini::net::EventLoop loop;
        const uint16_t port = 19201;
        mini::net::Acceptor acceptor(&loop, mini::net::InetAddress(port, true), true);

        int acceptedFd = -1;
        std::string peerIp;
        acceptor.setNewConnectionCallback([&](int sockfd, const mini::net::InetAddress& peerAddr) {
            acceptedFd = sockfd;
            peerIp = peerAddr.toIp();
            loop.quit();
        });

        assert(!acceptor.listening());
        acceptor.listen();
        assert(acceptor.listening());

        std::thread client([port] {
            std::this_thread::sleep_for(30ms);
            const int fd = connectTo(port);
            std::this_thread::sleep_for(50ms);
            ::close(fd);
        });

        loop.loop();
        client.join();

        assert(acceptedFd >= 0);
        assert(peerIp == "127.0.0.1");
        ::close(acceptedFd);
    }

    // Contract 2: no callback means accepted fd is closed explicitly (no leak)
    {
        mini::net::EventLoop loop;
        const uint16_t port = 19202;
        mini::net::Acceptor acceptor(&loop, mini::net::InetAddress(port, true), true);
        // deliberately no setNewConnectionCallback

        acceptor.listen();

        std::thread client([port] {
            std::this_thread::sleep_for(30ms);
            const int fd = connectTo(port);
            std::this_thread::sleep_for(50ms);
            ::close(fd);
        });

        // let the accept event fire, then quit
        loop.runAfter(100ms, [&loop] { loop.quit(); });
        loop.loop();
        client.join();
        // if Acceptor leaks fds here it would eventually exhaust the fd table,
        // but for a single-run test we just verify it does not crash/hang
    }

    // Contract 3: destroy-before-listen path is safe
    {
        mini::net::EventLoop loop;
        const uint16_t port = 19203;
        {
            mini::net::Acceptor acceptor(&loop, mini::net::InetAddress(port, true), true);
            // destroy without calling listen()
        }
        // must not crash — Channel was never added to Poller
    }

    // Contract 4: Acceptor destruction after listen correctly removes Channel from Poller
    {
        mini::net::EventLoop loop;
        const uint16_t port = 19204;
        {
            mini::net::Acceptor acceptor(&loop, mini::net::InetAddress(port, true), true);
            acceptor.listen();
            // destroy while listening — destructor must disableAll + remove
        }
        // must not crash; loop should be clean for further use
        bool ranAfter = false;
        loop.runInLoop([&] { ranAfter = true; });
        assert(ranAfter);
    }

    // Contract 5: multiple accepts in one readable event batch
    {
        mini::net::EventLoop loop;
        const uint16_t port = 19205;
        mini::net::Acceptor acceptor(&loop, mini::net::InetAddress(port, true), true);

        int acceptCount = 0;
        std::vector<int> acceptedFds;
        acceptor.setNewConnectionCallback([&](int sockfd, const mini::net::InetAddress&) {
            ++acceptCount;
            acceptedFds.push_back(sockfd);
        });
        acceptor.listen();

        std::thread clients([port] {
            std::this_thread::sleep_for(30ms);
            int fd1 = connectTo(port);
            int fd2 = connectTo(port);
            int fd3 = connectTo(port);
            std::this_thread::sleep_for(80ms);
            ::close(fd1);
            ::close(fd2);
            ::close(fd3);
        });

        loop.runAfter(150ms, [&loop] { loop.quit(); });
        loop.loop();
        clients.join();

        assert(acceptCount == 3);
        for (int fd : acceptedFds) {
            ::close(fd);
        }
    }

    return 0;
}
