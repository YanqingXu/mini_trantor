#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <future>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

uint16_t allocateTestPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    const int bound = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    assert(bound == 0);

    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    const int named = ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    assert(named == 0);

    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

std::string makeFrame(int seq) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "%06d|", seq);
    return buf;
}

void clientWorker(
    uint16_t port,
    int perWindow,
    int startSeq,
    const std::vector<std::string>& expectedFrames,
    std::mutex& readyMu,
    std::condition_variable& readyCv,
    int& readyCount,
    bool& windowGo,
    std::promise<void> done) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);

    assert(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

    timeval timeout{};
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    assert(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

    {
        std::lock_guard lock(readyMu);
        ++readyCount;
    }
    readyCv.notify_one();

    {
        std::unique_lock lock(readyMu);
        readyCv.wait(lock, [&] { return windowGo; });
    }

    const std::size_t frameLen = expectedFrames[0].size();
    const std::size_t expectedBytes = frameLen * static_cast<std::size_t>(perWindow);

    std::string buffer;
    buffer.reserve(expectedBytes);
    while (buffer.size() < expectedBytes) {
        char readBuf[128];
        const ssize_t n = ::recv(fd, readBuf, sizeof(readBuf), 0);
        assert(n > 0);
        buffer.append(readBuf, static_cast<std::size_t>(n));
    }

    assert(buffer.size() == expectedBytes);
    for (int i = 0; i < perWindow; ++i) {
        const auto& expect = expectedFrames[startSeq + i];
        const auto offset = static_cast<std::size_t>(i) * frameLen;
        const auto actual = buffer.substr(offset, frameLen);
        assert(actual == expect);
    }

    assert(::close(fd) == 0);
    done.set_value();
}

}  // namespace

int main() {
    const uint16_t port = allocateTestPort();
    constexpr int kClientCount = 8;
    constexpr int kWindowCount = 4;
    constexpr int kPerWindow = 250;
    constexpr int kTotalBroadcast = kWindowCount * kPerWindow;

    std::vector<std::string> expectedFrames;
    expectedFrames.reserve(kTotalBroadcast);
    for (int i = 0; i < kTotalBroadcast; ++i) {
        expectedFrames.push_back(makeFrame(i));
    }

    mini::net::EventLoopThread loopThread;
    auto* baseLoop = loopThread.startLoop();

    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "broadcast-stress-reconnect", true);
    auto* serverRaw = server.get();
    server->setThreadNum(3);

    std::mutex serverMu;
    std::condition_variable serverCv;
    int serverConnectedCount = 0;
    serverRaw->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
        if (!conn->connected()) {
            return;
        }
        {
            std::lock_guard lock(serverMu);
            ++serverConnectedCount;
        }
        serverCv.notify_all();
    });

    auto started = std::make_shared<std::promise<void>>();
    auto startedFuture = started->get_future();
    baseLoop->queueInLoop([serverRaw, started] {
        serverRaw->start();
        started->set_value();
    });
    assert(startedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    for (int window = 0; window < kWindowCount; ++window) {
        int readyCount = 0;
        bool windowGo = false;
        std::mutex readyMu;
        std::condition_variable readyCv;

        std::vector<std::thread> clients;
        clients.reserve(kClientCount);
        std::vector<std::promise<void>> clientDone;
        std::vector<std::future<void>> clientFutures;
        clientDone.reserve(kClientCount);
        clientFutures.reserve(kClientCount);

        const int startSeq = window * kPerWindow;

        for (int i = 0; i < kClientCount; ++i) {
            clientDone.emplace_back();
            clientFutures.push_back(clientDone.back().get_future());
            clients.emplace_back(
                clientWorker,
                port,
                kPerWindow,
                startSeq,
                std::cref(expectedFrames),
                std::ref(readyMu),
                std::ref(readyCv),
                std::ref(readyCount),
                std::ref(windowGo),
                std::move(clientDone.back()));
        }

    {
        std::unique_lock lock(readyMu);
        const bool allClientsReady = readyCv.wait_for(
            lock,
            std::chrono::seconds(3),
            [&] { return readyCount == kClientCount; });
        assert(allClientsReady);
        windowGo = true;
        readyCv.notify_all();
    }

        {
            std::unique_lock lock(serverMu);
            const int targetConnected = (window + 1) * kClientCount;
            const bool allRegistered = serverCv.wait_for(
                lock,
                std::chrono::seconds(3),
                [&] { return serverConnectedCount >= targetConnected; });
            assert(allRegistered);
        }

        for (int i = 0; i < kPerWindow; ++i) {
            serverRaw->broadcast(expectedFrames[startSeq + i]);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }

        for (auto& future : clientFutures) {
            assert(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        }
        for (auto& t : clients) {
            t.join();
        }
    }

    auto stopped = std::make_shared<std::promise<void>>();
    auto stoppedFuture = stopped->get_future();
    baseLoop->queueInLoop([serverRaw, stopped]() {
        serverRaw->stop();
        stopped->set_value();
    });
    assert(stoppedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    auto serverOwner = std::make_shared<std::unique_ptr<mini::net::TcpServer>>(std::move(server));
    auto destroyed = std::make_shared<std::promise<void>>();
    auto destroyedFuture = destroyed->get_future();
    baseLoop->queueInLoop([serverOwner, baseLoop, destroyed] {
        if (serverOwner && *serverOwner) {
            serverOwner->reset();
        }
        baseLoop->quit();
        destroyed->set_value();
    });
    assert(destroyedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    return 0;
}
