// Intent: connector.intent.md. Terminal events publish a cleaned attempt before
// user re-entry; queued generations cannot revive stopped Channels or timers.
// Gate: owner EventLoop; external shared Connector owner; event/fd callbacks may
// stop/restart/replace/release; cross-thread controls marshal; this file verifies.
#include "mini/net/Connector.h"
#include "mini/net/EventLoop.h"
#include "mini/net/Socket.h"
#include "mini/net/SocketsOps.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string_view>
#include <utility>

using namespace mini::net;
using namespace std::chrono_literals;

namespace {

struct Endpoint {
    Socket socket{sockets::createNonblockingOrDie(AF_INET)};
    InetAddress address;
    explicit Endpoint(bool listening = false) {
        socket.bindAddress(InetAddress(0, true));
        address = InetAddress(sockets::getLocalAddr(socket.fd()));
        if (listening) { socket.listen(); }
    }
};

void watchdog(EventLoop& loop) {
    loop.runAfter(5s, [] { assert(false && "Connector contract timed out"); });
}

void resetProgressDeadline(EventLoop& loop, TimerId& deadline) {
    if (deadline.valid()) { loop.cancel(deadline); }
    // Windows reports a local refused connect after about two seconds. The
    // three-attempt contracts bound each stage, not their combined duration.
    deadline = loop.runAfter(5s, [] {
        assert(false && "Connector attempt made no progress");
    });
}

void terminalState() {
    EventLoop loop;
    Endpoint refused;
    auto connector = std::make_shared<Connector>(&loop, refused.address);
    int failed = 0;
    connector->setConnectorEventCallback([&](const auto&, ConnectorEvent event) {
        if (event == ConnectorEvent::ConnectFailed) {
            ++failed;
            assert(connector->state() == Connector::kDisconnected);
            connector->stop(); // terminal cleanup must already be complete
            loop.queueInLoop([&] { loop.quit(); });
        }
    });
    connector->start();
    watchdog(loop);
    loop.loop();
    assert(failed == 1);
}

void disabledRetry() {
    EventLoop loop;
    Endpoint refused;
    ConnectorOptions options;
    options.enableRetry = false;
    auto connector = std::make_shared<Connector>(&loop, refused.address, options);
    int failed = 0, retries = 0;
    connector->setConnectorEventCallback([&](const auto&, ConnectorEvent event) {
        if (event == ConnectorEvent::RetryScheduled) { ++retries; }
        if (event == ConnectorEvent::ConnectFailed) {
            ++failed;
            loop.queueInLoop([&] {
                assert(retries == 0);
                connector->stop();
                loop.quit();
            });
        }
    });
    connector->start();
    watchdog(loop);
    loop.loop();
    assert(failed == 1 && retries == 0);
}

void stopFromAttempt() {
    EventLoop loop;
    Endpoint listener(true);
    auto connector = std::make_shared<Connector>(&loop, listener.address);
    int attempts = 0;
    connector->setNewConnectionCallback([](SocketFd) {
        assert(false && "stopped attempt delivered an fd");
    });
    connector->setConnectorEventCallback([&](const auto&, ConnectorEvent event) {
        assert(event == ConnectorEvent::ConnectAttempt);
        ++attempts;
        connector->stop();
        loop.queueInLoop([&] {
            assert(connector->state() == Connector::kDisconnected);
            sockaddr_storage peer{};
            const auto accepted = sockets::accept(listener.socket.fd(), &peer);
            assert(!sockets::isValid(accepted)); // stop precedes socket creation
            loop.quit();
        });
    });
    connector->start();
    watchdog(loop);
    loop.loop();
    assert(attempts == 1);
}

void restartFromFailure() {
    EventLoop loop;
    Endpoint refused;
    auto connector = std::make_shared<Connector>(&loop, refused.address);
    int attempts = 0, failed = 0;
    TimerId progressDeadline;
    connector->setConnectorEventCallback([&](const auto&, ConnectorEvent event) {
        if (event == ConnectorEvent::ConnectAttempt) {
            ++attempts;
            assert(attempts <= 3);
            resetProgressDeadline(loop, progressDeadline);
        }
        if (event == ConnectorEvent::ConnectFailed) {
            ++failed;
            assert(connector->state() == Connector::kDisconnected);
            if (failed < 3) {
                connector->restart();
                connector->start(); // no duplicate queued attempt
                connector->start();
            } else {
                connector->stop();
                loop.cancel(progressDeadline);
                progressDeadline = {};
                loop.queueInLoop([&] { loop.quit(); });
            }
        }
    });
    connector->start();
    connector->start();
    resetProgressDeadline(loop, progressDeadline);
    loop.loop();
    assert(attempts == 3 && failed == 3);
}

void stopAndStartBeforeRetirement() {
    EventLoop loop;
    Endpoint listener(true);
    auto connector = std::make_shared<Connector>(&loop, listener.address);
    int attempts = 0, delivered = 0;
    connector->setConnectorEventCallback([&](const auto&, ConnectorEvent event) {
        if (event == ConnectorEvent::ConnectAttempt) { ++attempts; }
    });
    connector->setNewConnectionCallback([&](SocketFd fd) {
        sockets::close(fd);
        ++delivered;
        if (attempts == 2) {
            connector->stop();
            loop.quit();
        }
    });
    connector->start();
    // These two functors share one batch: no readiness dispatch can occur
    // between the queued first start and retirement of its live Channel.
    loop.queueInLoop([&] {
        assert(attempts == 1 && connector->state() == Connector::kConnecting);
        connector->stop();
        connector->start();
        connector->restart(); // supersedes the queued replacement
        connector->start();
    });
    watchdog(loop);
    loop.loop();
    assert(attempts == 2);
    assert(delivered == 1);
}

void automaticRetryAndRestart() {
    EventLoop loop;
    Endpoint refused;
    ConnectorOptions options;
    options.enableRetry = true;
    options.initRetryDelay = 1ms;
    options.maxRetryDelay = 2ms;
    auto connector = std::make_shared<Connector>(&loop, refused.address, options);
    int attempts = 0, failures = 0, retries = 0;
    TimerId progressDeadline;
    connector->setConnectorEventCallback([&](const auto&, ConnectorEvent event) {
        if (event == ConnectorEvent::ConnectAttempt) {
            ++attempts;
            assert(attempts <= 3);
            resetProgressDeadline(loop, progressDeadline);
        }
        if (event == ConnectorEvent::RetryScheduled) {
            if (++retries == 1) { connector->restart(); }
        }
        if (event == ConnectorEvent::ConnectFailed && ++failures == 3) {
            connector->stop();
            loop.cancel(progressDeadline);
            progressDeadline = {};
            loop.queueInLoop([&] { loop.quit(); });
        }
    });
    connector->start();
    resetProgressDeadline(loop, progressDeadline);
    loop.loop();
    assert(attempts == 3 && failures == 3 && retries == 2);
}

void callbackCopiesAndSuccessRestart() {
    EventLoop loop;
    Endpoint listener(true);
    auto connector = std::make_shared<Connector>(&loop, listener.address);
    int successes = 0, delivered = 0;
    auto eventCapture = std::make_shared<int>(1);
    std::weak_ptr<int> eventLifetime = eventCapture;
    ConnectorEventCallback replacement = [&](const auto&, ConnectorEvent event) {
        if (event == ConnectorEvent::ConnectSuccess) { ++successes; }
    };
    connector->setConnectorEventCallback(
        [&, capture = std::move(eventCapture)](const auto&, ConnectorEvent event) {
            if (event == ConnectorEvent::ConnectSuccess) {
                ++successes;
                connector->setConnectorEventCallback(replacement);
                assert(!eventLifetime.expired());
                assert(*capture == 1);
                connector->restart(); // suppress this old success fd
            }
        });
    auto fdCapture = std::make_shared<int>(2);
    std::weak_ptr<int> fdLifetime = fdCapture;
    connector->setNewConnectionCallback([&, capture = std::move(fdCapture)](SocketFd fd) {
        ++delivered;
        sockets::close(fd);
        connector->setNewConnectionCallback({});
        assert(!fdLifetime.expired());
        assert(*capture == 2);
        connector->stop();
        loop.queueInLoop([&] { loop.quit(); });
    });
    connector->start();
    watchdog(loop);
    loop.loop();
    assert(successes == 2 && delivered == 1);
    assert(eventLifetime.expired() && fdLifetime.expired());
}

void stopInstalledRetry() {
    EventLoop loop;
    Endpoint refused;
    ConnectorOptions options;
    options.enableRetry = true;
    options.initRetryDelay = 10s;
    options.maxRetryDelay = 10s;
    auto connector = std::make_shared<Connector>(&loop, refused.address, options);
    std::weak_ptr<Connector> lifetime = connector;
    int retries = 0;
    connector->setConnectorEventCallback([&](const auto&, ConnectorEvent event) {
        if (event == ConnectorEvent::RetryScheduled) {
            ++retries;
            connector->stop();
            connector.reset();
            loop.queueInLoop([&] {
                assert(lifetime.expired()); // neither timer nor retired Channel owns it
                loop.quit();
            });
        }
    });
    connector->start();
    watchdog(loop);
    loop.loop();
    assert(retries == 1 && lifetime.expired());
}

void releaseOwnerInAttempt() {
    EventLoop loop;
    Endpoint listener(true);
    auto connector = std::make_shared<Connector>(&loop, listener.address);
    std::weak_ptr<Connector> lifetime = connector;
    connector->setConnectorEventCallback([&](const auto&, ConnectorEvent event) {
        assert(event == ConnectorEvent::ConnectAttempt);
        connector->stop();
        connector.reset();
        loop.queueInLoop([&] {
            assert(lifetime.expired());
            loop.quit();
        });
    });
    connector->start();
    watchdog(loop);
    loop.loop();
    assert(lifetime.expired());
}

void releasePendingOwner() {
    EventLoop loop;
    Endpoint listener(true);
    auto connector = std::make_shared<Connector>(&loop, listener.address);
    std::weak_ptr<Connector> lifetime = connector;
    connector->start();
    loop.queueInLoop([&] {
        assert(connector->state() == Connector::kConnecting);
        connector.reset(); // live connecting Channel is cleaned by destructor
        assert(lifetime.expired());
        loop.quit();
    });
    watchdog(loop);
    loop.loop();
    assert(lifetime.expired());
}

void validateConfiguration() {
    EventLoop loop;
    const InetAddress address(1, true);
    ConnectorOptions options;
    options.initRetryDelay = 0ms;
    bool rejected = false;
    try { Connector connector(&loop, address, options); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    Connector connector(&loop, address);
    for (auto delays : {std::pair{0ms, 1ms}, std::pair{2ms, 1ms}}) {
        rejected = false;
        try { connector.setRetryDelay(delays.first, delays.second); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
}

} // namespace

int main(int argc, char** argv) {
    const auto selected = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
    const auto run = [&](std::string_view name, auto test) {
        if (selected.empty() || selected == name) {
            std::printf("Connector contract: %.*s\n", static_cast<int>(name.size()), name.data());
            std::fflush(stdout);
            test();
        }
    };
    run("terminal", terminalState);
    run("no_retry", disabledRetry);
    run("stop_attempt", stopFromAttempt);
    run("restart_failure", restartFromFailure);
    run("retirement", stopAndStartBeforeRetirement);
    run("auto_retry", automaticRetryAndRestart);
    run("callback_copy", callbackCopiesAndSuccessRestart);
    run("stop_retry", stopInstalledRetry);
    run("release_owner", releaseOwnerInAttempt);
    run("release_pending", releasePendingOwner);
    run("configuration", validateConfiguration);
}
