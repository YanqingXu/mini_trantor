// TcpClient DNS candidate contracts. The linker wraps OS resolution and socket creation;
// TcpClient, Connector and DnsResolver retain their production public interfaces.
//
// Gate: client/Connector state and hooks belong to this test's EventLoop thread.
// The fixture owns client, resolver and listening sockets; resolver joins workers.
// ConnectFailed hooks may stop/disconnect/destroy the client. DNS workers only
// return address values; the resolver marshals completion to the owner loop.

#include "mini/net/DnsResolver.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpClient.h"
#include "mini/net/TcpConnection.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using mini::net::ConnectorEvent;
using mini::net::InetAddress;

namespace {

struct Reply {
    explicit Reply(std::vector<InetAddress> values) : addresses(std::move(values)) {}
    std::vector<InetAddress> addresses;
    std::promise<void> entered;
    std::shared_future<void> release;
};

struct ResolverScenario {
    std::vector<std::unique_ptr<Reply>> replies;
    std::atomic<unsigned> calls{0};
    int unavailableSocketFamily{AF_UNSPEC};
    unsigned rejectedSockets{0};  // socket() runs on the fixture/owner thread only
};

// Installed before starting any worker, cleared only after resolver destruction.
ResolverScenario* currentScenario = nullptr;

struct FakeAddress {
    addrinfo info{};
    sockaddr_storage address{};
};

class BoundSocket {
public:
    BoundSocket(const char* ip, uint16_t port, bool listening)
        : address_(ip, port), fd_(::socket(address_.family(), SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        assert(fd_ >= 0);
        if (address_.isIpv6()) {
            const int onlyIpv6 = 1;
            const int configured = ::setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY,
                                               &onlyIpv6, sizeof(onlyIpv6));
            assert(configured == 0);
        }
        const int bound = ::bind(fd_, address_.getSockAddr(), address_.getSockAddrLen());
        assert(bound == 0);
        sockaddr_storage actual{};
        socklen_t size = sizeof(actual);
        const int named = ::getsockname(fd_, reinterpret_cast<sockaddr*>(&actual), &size);
        assert(named == 0);
        address_ = InetAddress(actual);
        if (listening) {
            const int started = ::listen(fd_, 8);
            assert(started == 0);
        }
    }

    ~BoundSocket() { ::close(fd_); }
    const InetAddress& address() const { return address_; }
    void closeAcceptedConnection() {
        const int peer = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
        assert(peer >= 0);
        ::close(peer);
    }

private:
    InetAddress address_;
    int fd_;
};

bool supportsIpv6Loopback() {
    const int fd = ::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        assert(errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT);
        std::printf("  IPv6 loopback unavailable: socket errno %d\n", errno);
        return false;
    }
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_loopback;
    const int bound = ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    const int error = errno;
    ::close(fd);
    if (bound != 0) {
        assert(error == EADDRNOTAVAIL || error == EAFNOSUPPORT);
        std::printf("  IPv6 loopback unavailable: bind errno %d\n", error);
        return false;
    }
    return true;
}

struct Trace {
    std::vector<std::string> attempts;
    std::vector<std::string> failures;
    unsigned successes{0};
    unsigned retries{0};

    void record(const InetAddress& address, ConnectorEvent event) {
        if (event == ConnectorEvent::ConnectAttempt) attempts.push_back(address.toIpPort());
        if (event == ConnectorEvent::ConnectFailed) failures.push_back(address.toIpPort());
        if (event == ConnectorEvent::ConnectSuccess) ++successes;
        if (event == ConnectorEvent::RetryScheduled) ++retries;
    }
};

void runWithDeadline(mini::net::EventLoop& loop) {
    bool expired = false;
    const auto deadline = loop.runAfter(5s, [&] { expired = true; loop.quit(); });
    loop.loop();
    loop.cancel(deadline);
    assert(!expired);
}

void fallbackInOrder(const char* failedIp, const char* successfulIp) {
    BoundSocket listener(successfulIp, 0, true);
    // Keeping a bound, non-listening socket reserves the exact refused endpoint.
    BoundSocket refused(failedIp, listener.address().port(), false);
    ResolverScenario scenario;
    scenario.replies.push_back(std::make_unique<Reply>(
        std::vector<InetAddress>{refused.address(), listener.address()}));
    currentScenario = &scenario;
    {
        mini::net::EventLoop loop;
        auto resolver = std::make_shared<mini::net::DnsResolver>(1);
        mini::net::TcpClient client(&loop, "candidates.mini-test.invalid",
                                   listener.address().port(), "fallback", resolver);
        Trace trace;
        bool connected = false;
        unsigned connectedEvents = 0;
        unsigned disconnectedEvents = 0;
        client.setConnectorEventCallback([&](const auto& address, auto event) {
            assert(loop.isInLoopThread());
            trace.record(address, event);
            // An intermediate candidate failure still belongs to this round.
            if (event == ConnectorEvent::ConnectFailed) client.connect();
        });
        client.setConnectionEventCallback([&](const auto&, auto event) {
            assert(loop.isInLoopThread());
            if (event == mini::net::ConnectionEvent::Connected) ++connectedEvents;
            if (event == mini::net::ConnectionEvent::Disconnected) ++disconnectedEvents;
        });
        client.setConnectionCallback([&](const auto& connection) {
            assert(loop.isInLoopThread());
            if (!connection->connected()) {
                loop.quit();
                return;
            }
            assert(connection->peerAddress().toIpPort() == listener.address().toIpPort());
            connected = true;
            listener.closeAcceptedConnection();
        });
        client.connect();
        runWithDeadline(loop);
        assert(connected);
        assert((trace.attempts == std::vector<std::string>{refused.address().toIpPort(),
                                                        listener.address().toIpPort()}));
        assert((trace.failures == std::vector<std::string>{refused.address().toIpPort()}));
        assert(trace.successes == 1);
        assert(trace.retries == 0);
        assert(scenario.calls == 1);
        assert(connectedEvents == 1);
        assert(disconnectedEvents == 1);
    }
    currentScenario = nullptr;
    std::printf("  PASS: ordered fallback %s -> %s\n", failedIp, successfulIp);
}

void unavailableSocketFamilyFallsBack() {
    BoundSocket listener("127.0.0.1", 0, true);
    const InetAddress unavailable("::1", listener.address().port());
    ResolverScenario scenario;
    scenario.unavailableSocketFamily = AF_INET6;
    scenario.replies.push_back(std::make_unique<Reply>(
        std::vector<InetAddress>{unavailable, listener.address()}));
    currentScenario = &scenario;
    {
        mini::net::EventLoop loop;
        auto resolver = std::make_shared<mini::net::DnsResolver>(1);
        mini::net::TcpClient client(&loop, "candidates.mini-test.invalid",
                                   listener.address().port(), "unavailable-family", resolver);
        Trace trace;
        bool connected = false;
        client.setConnectorEventCallback([&](const auto& address, auto event) {
            assert(loop.isInLoopThread());
            trace.record(address, event);
        });
        client.setConnectionCallback([&](const auto& connection) {
            if (!connection->connected()) return;
            assert(connection->peerAddress().toIpPort() == listener.address().toIpPort());
            connected = true;
            loop.quit();
        });
        client.connect();
        runWithDeadline(loop);
        assert(connected);
        assert((trace.attempts == std::vector<std::string>{unavailable.toIpPort(),
                                                        listener.address().toIpPort()}));
        assert((trace.failures == std::vector<std::string>{unavailable.toIpPort()}));
        assert(trace.successes == 1);
        assert(trace.retries == 0);
        assert(scenario.calls == 1);
        assert(scenario.rejectedSockets == 1);
    }
    currentScenario = nullptr;
    std::printf("  PASS: unavailable IPv6 socket family falls back to IPv4\n");
}

void allCandidatesFailOnce() {
    BoundSocket first("127.0.0.1", 0, false);
    BoundSocket second("127.0.0.2", first.address().port(), false);
    ResolverScenario scenario;
    scenario.replies.push_back(std::make_unique<Reply>(
        std::vector<InetAddress>{first.address(), second.address()}));
    currentScenario = &scenario;
    {
        mini::net::EventLoop loop;
        auto resolver = std::make_shared<mini::net::DnsResolver>(1);
        mini::net::TcpClient client(&loop, "candidates.mini-test.invalid",
                                   first.address().port(), "all-refused", resolver);
        Trace trace;
        client.setConnectorEventCallback([&](const auto& address, auto event) {
            trace.record(address, event);
            // RetryScheduled is observable immediately when a retry is armed.
            assert(trace.retries == 0);
            if (trace.failures.size() == 2) loop.queueInLoop([&] { loop.quit(); });
        });
        client.setConnectionCallback([](const auto&) { assert(false); });
        client.connect();
        runWithDeadline(loop);
        const std::vector<std::string> expected{first.address().toIpPort(), second.address().toIpPort()};
        assert(trace.attempts == expected);
        assert(trace.failures == expected);
        assert(trace.successes == 0);
        assert(scenario.calls == 1);
    }
    currentScenario = nullptr;
    std::printf("  PASS: all candidates fail once without automatic retry\n");
}

enum class FailureAction { Stop, Disconnect, Destroy };

void failureHookCancelsCandidates(FailureAction action) {
    BoundSocket listener("127.0.0.1", 0, true);
    BoundSocket refused("127.0.0.2", listener.address().port(), false);
    ResolverScenario scenario;
    scenario.replies.push_back(std::make_unique<Reply>(
        std::vector<InetAddress>{refused.address(), listener.address()}));
    currentScenario = &scenario;
    {
        mini::net::EventLoop loop;
        auto resolver = std::make_shared<mini::net::DnsResolver>(1);
        auto client = std::make_unique<mini::net::TcpClient>(
            &loop, "candidates.mini-test.invalid", listener.address().port(), "hook-cancel", resolver);
        Trace trace;
        client->setConnectorEventCallback([&](const auto& address, auto event) {
            trace.record(address, event);
            if (event != ConnectorEvent::ConnectFailed) return;
            if (action == FailureAction::Stop) client->stop();
            if (action == FailureAction::Disconnect) client->disconnect();
            if (action == FailureAction::Destroy) client.reset();
            loop.queueInLoop([&] { loop.quit(); });
        });
        client->setConnectionCallback([](const auto&) { assert(false); });
        client->connect();
        runWithDeadline(loop);
        assert((trace.attempts == std::vector<std::string>{refused.address().toIpPort()}));
        assert(trace.failures.size() == 1);
        assert(trace.successes == 0);
        assert(trace.retries == 0);
        assert(scenario.calls == 1);
    }
    currentScenario = nullptr;
    std::printf("  PASS: failure hook cancels later candidates (%d)\n", static_cast<int>(action));
}

void repeatedConnectDoesNotResolveTwice() {
    BoundSocket listener("127.0.0.1", 0, true);
    ResolverScenario scenario;
    scenario.replies.push_back(std::make_unique<Reply>(
        std::vector<InetAddress>{listener.address()}));
    std::promise<void> release;
    scenario.replies[0]->release = release.get_future().share();
    auto entered = scenario.replies[0]->entered.get_future();
    currentScenario = &scenario;
    {
        mini::net::EventLoop loop;
        auto resolver = std::make_shared<mini::net::DnsResolver>(1);
        mini::net::TcpClient client(&loop, "candidates.mini-test.invalid",
                                   listener.address().port(), "duplicate-connect", resolver);
        unsigned connected = 0;
        client.setConnectionCallback([&](const auto& connection) {
            if (!connection->connected()) return;
            ++connected;
            loop.quit();
        });
        client.connect();
        const auto status = entered.wait_for(5s);
        assert(status == std::future_status::ready);
        client.connect();
        release.set_value();
        runWithDeadline(loop);
        assert(connected == 1);
    }
    // Destruction above joins the resolver, so no queued duplicate can hide here.
    assert(scenario.calls == 1);
    currentScenario = nullptr;
    std::printf("  PASS: repeated connect keeps one pending resolution\n");
}

void newConnectDiscardsOldResolution() {
    BoundSocket listener("127.0.0.1", 0, true);
    BoundSocket oldAddress("127.0.0.2", listener.address().port(), false);
    ResolverScenario scenario;
    scenario.replies.push_back(std::make_unique<Reply>(std::vector<InetAddress>{oldAddress.address()}));
    scenario.replies.push_back(std::make_unique<Reply>(std::vector<InetAddress>{listener.address()}));
    std::promise<void> releaseOld;
    std::promise<void> releaseNew;
    scenario.replies[0]->release = releaseOld.get_future().share();
    scenario.replies[1]->release = releaseNew.get_future().share();
    auto oldEntered = scenario.replies[0]->entered.get_future();
    auto newEntered = scenario.replies[1]->entered.get_future();
    currentScenario = &scenario;
    {
        mini::net::EventLoop loop;
        auto resolver = std::make_shared<mini::net::DnsResolver>(1);
        mini::net::TcpClient client(&loop, "candidates.mini-test.invalid",
                                   listener.address().port(), "new-generation", resolver);
        Trace trace;
        bool connected = false;
        client.setConnectorEventCallback([&](const auto& address, auto event) { trace.record(address, event); });
        client.setConnectionCallback([&](const auto& connection) {
            if (!connection->connected()) return;
            connected = true;
            loop.quit();
        });
        client.connect();
        const auto oldStatus = oldEntered.wait_for(5s);
        assert(oldStatus == std::future_status::ready);
        client.stop();
        client.connect();
        releaseOld.set_value();
        const auto newStatus = newEntered.wait_for(5s);
        assert(newStatus == std::future_status::ready);
        // One resolver worker has already posted the old result before entering
        // the blocked new getaddrinfo call. The second marker also follows any
        // Connector::start work queued by incorrectly accepting that old result.
        loop.queueInLoop([&] {
            loop.queueInLoop([&] {
                assert(trace.attempts.empty());
                releaseNew.set_value();
            });
        });
        runWithDeadline(loop);
        assert(connected);
        assert((trace.attempts == std::vector<std::string>{listener.address().toIpPort()}));
        assert(trace.failures.empty());
        assert(trace.successes == 1);
    }
    assert(scenario.calls == 2);
    currentScenario = nullptr;
    std::printf("  PASS: new connect ignores the earlier queued DNS result\n");
}

void connectedCallbackKeepsOneConnection() {
    BoundSocket listener("127.0.0.1", 0, true);
    ResolverScenario scenario;
    scenario.replies.push_back(std::make_unique<Reply>(std::vector<InetAddress>{listener.address()}));
    currentScenario = &scenario;
    {
        mini::net::EventLoop loop;
        auto resolver = std::make_shared<mini::net::DnsResolver>(1);
        mini::net::TcpClient client(&loop, "candidates.mini-test.invalid",
                                   listener.address().port(), "connected-reentry", resolver);
        Trace trace;
        unsigned connected = 0;
        client.setConnectorEventCallback([&](const auto& address, auto event) { trace.record(address, event); });
        client.setConnectionCallback([&](const auto& connection) {
            if (!connection->connected()) return;
            ++connected;
            assert(connected == 1);
            client.connect();
            client.connect();
            assert(client.connection() == connection);
            loop.queueInLoop([&] { loop.quit(); });
        });
        client.connect();
        runWithDeadline(loop);
        assert(connected == 1);
        assert((trace.attempts == std::vector<std::string>{listener.address().toIpPort()}));
        assert(trace.successes == 1);
    }
    assert(scenario.calls == 1);
    currentScenario = nullptr;
    std::printf("  PASS: connect from Connected callback keeps one connection\n");
}

void disconnectedCallbackRequestsNewConnection() {
    BoundSocket listener("127.0.0.1", 0, true);
    ResolverScenario scenario;
    scenario.replies.push_back(std::make_unique<Reply>(std::vector<InetAddress>{listener.address()}));
    scenario.replies.push_back(std::make_unique<Reply>(std::vector<InetAddress>{listener.address()}));
    currentScenario = &scenario;
    {
        mini::net::EventLoop loop;
        auto resolver = std::make_shared<mini::net::DnsResolver>(1);
        mini::net::TcpClient client(&loop, "candidates.mini-test.invalid",
                                   listener.address().port(), "disconnected-reentry", resolver);
        assert(!client.retry());
        Trace trace;
        unsigned connected = 0;
        unsigned disconnected = 0;
        client.setConnectorEventCallback([&](const auto& address, auto event) { trace.record(address, event); });
        client.setConnectionCallback([&](const auto& connection) {
            assert(loop.isInLoopThread());
            if (connection->connected()) {
                ++connected;
                if (connected == 1) listener.closeAcceptedConnection();
                else loop.quit();
                return;
            }
            ++disconnected;
            assert(disconnected == 1);
            assert(client.connection() == connection);
            client.connect();
            // The closing connection remains published throughout its callback;
            // the replacement starts only after removeConnection completes.
            assert(client.connection() == connection);
            assert(trace.attempts.size() == 1);
        });
        client.connect();
        runWithDeadline(loop);
        assert(connected == 2);
        assert(disconnected == 1);
        assert((trace.attempts == std::vector<std::string>{listener.address().toIpPort(),
                                                        listener.address().toIpPort()}));
        assert(trace.successes == 2);
        assert(trace.failures.empty());
        assert(trace.retries == 0);
    }
    assert(scenario.calls == 2);
    currentScenario = nullptr;
    std::printf("  PASS: connect from Disconnected callback waits for old removal\n");
}

void finalFailureHookRequestsNewRound() {
    BoundSocket listener("127.0.0.1", 0, true);
    BoundSocket refused("127.0.0.2", listener.address().port(), false);
    ResolverScenario scenario;
    scenario.replies.push_back(std::make_unique<Reply>(std::vector<InetAddress>{refused.address()}));
    scenario.replies.push_back(std::make_unique<Reply>(std::vector<InetAddress>{listener.address()}));
    currentScenario = &scenario;
    {
        mini::net::EventLoop loop;
        auto resolver = std::make_shared<mini::net::DnsResolver>(1);
        mini::net::TcpClient client(&loop, "candidates.mini-test.invalid",
                                   listener.address().port(), "failure-reconnect", resolver);
        Trace trace;
        bool connected = false;
        client.setConnectorEventCallback([&](const auto& address, auto event) {
            assert(loop.isInLoopThread());
            trace.record(address, event);
            if (event == ConnectorEvent::ConnectFailed) {
                assert(trace.failures.size() == 1);
                // The last candidate has terminated before this notification.
                // This explicit new round must survive old-attempt retirement.
                client.connect();
            }
        });
        client.setConnectionCallback([&](const auto& connection) {
            if (!connection->connected()) return;
            assert(connection->peerAddress().toIpPort() == listener.address().toIpPort());
            connected = true;
            loop.quit();
        });
        client.connect();
        runWithDeadline(loop);
        assert(connected);
        assert((trace.attempts == std::vector<std::string>{refused.address().toIpPort(),
                                                        listener.address().toIpPort()}));
        assert((trace.failures == std::vector<std::string>{refused.address().toIpPort()}));
        assert(trace.successes == 1);
        assert(trace.retries == 0);
    }
    assert(scenario.calls == 2);
    currentScenario = nullptr;
    std::printf("  PASS: final failure hook may explicitly start a new DNS round\n");
}

}  // namespace

extern "C" int __real_socket(int domain, int type, int protocol);

extern "C" int __wrap_socket(int domain, int type, int protocol) {
    if (currentScenario && domain == currentScenario->unavailableSocketFamily) {
        ++currentScenario->rejectedSockets;
        errno = EAFNOSUPPORT;
        return -1;
    }
    return __real_socket(domain, type, protocol);
}

extern "C" int __wrap_getaddrinfo(const char* node, const char* service,
                                   const addrinfo* hints, addrinfo** result) {
    assert(currentScenario);
    assert(std::string(node) == "candidates.mini-test.invalid");
    assert(hints && hints->ai_family == AF_UNSPEC && hints->ai_socktype == SOCK_STREAM);
    const unsigned call = currentScenario->calls.fetch_add(1);
    auto& replies = currentScenario->replies;
    auto& reply = *replies[std::min<std::size_t>(call, replies.size() - 1)];
    if (call < replies.size()) reply.entered.set_value();
    if (reply.release.valid()) reply.release.wait();
    *result = nullptr;
    addrinfo** next = result;
    for (const auto& address : reply.addresses) {
        assert(address.port() == std::stoul(service));
        auto* fake = new FakeAddress;
        std::memcpy(&fake->address, address.getSockAddr(), address.getSockAddrLen());
        fake->info.ai_family = address.family();
        fake->info.ai_socktype = SOCK_STREAM;
        fake->info.ai_protocol = IPPROTO_TCP;
        fake->info.ai_addrlen = address.getSockAddrLen();
        fake->info.ai_addr = reinterpret_cast<sockaddr*>(&fake->address);
        *next = &fake->info;
        next = &fake->info.ai_next;
    }
    return 0;
}

extern "C" void __wrap_freeaddrinfo(addrinfo* result) {
    while (result) {
        auto* next = result->ai_next;
        delete reinterpret_cast<FakeAddress*>(result);
        result = next;
    }
}

int main(int argc, char** argv) {
    // The named modes allow preserving focused red evidence without disabling
    // any contract in the ordinary CTest run.
    const std::string mode = argc > 1 ? argv[1] : "all";
    if (mode == "all" || mode == "fallback") {
        fallbackInOrder("127.0.0.2", "127.0.0.1");
        if (supportsIpv6Loopback()) {
            fallbackInOrder("::1", "127.0.0.1");
            fallbackInOrder("127.0.0.1", "::1");
        }
    }
    if (mode == "all" || mode == "failure") allCandidatesFailOnce();
    if (mode == "all" || mode == "final_failure") finalFailureHookRequestsNewRound();
    if (mode == "all" || mode == "unavailable") unavailableSocketFamilyFallsBack();
    if (mode == "all" || mode == "hooks") {
        failureHookCancelsCandidates(FailureAction::Stop);
        failureHookCancelsCandidates(FailureAction::Disconnect);
        failureHookCancelsCandidates(FailureAction::Destroy);
    }
    if (mode == "all" || mode == "duplicate") repeatedConnectDoesNotResolveTwice();
    if (mode == "all" || mode == "generation") newConnectDiscardsOldResolution();
    if (mode == "all" || mode == "connected") connectedCallbackKeepsOneConnection();
    if (mode == "all" || mode == "disconnected") disconnectedCallbackRequestsNewConnection();
    std::printf("DNS candidate contracts passed.\n");
}
