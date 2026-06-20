// 集成级验证：Task-10（短时状态保留 / Sticky）
// 1) 窗口内重连应复用会话对象
// 2) 窗口外重连应得到新会话对象（老会话已回收）

#include "mini/game/SessionManager.h"
#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/ProtocolConnectionAdapter.h"
#include "mini/net/TcpServer.h"
#include "mini/net/TcpConnection.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <string_view>
#include <unistd.h>

namespace {

struct ServerSessionState {
    std::atomic<mini::net::transport::TransportSessionId> nextTransportId{
        mini::net::transport::kFirstTransportSessionId};
    std::unordered_map<std::string, mini::net::transport::TransportSessionId> connToTransport;
    std::mutex mutex;
};

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

void runAuthClient(uint16_t port, std::string_view token) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const int converted = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    assert(converted == 1);

    const int connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    assert(connected == 0);

    const ssize_t written = ::write(fd, token.data(), token.size());
    assert(written == static_cast<ssize_t>(token.size()));

    char reply[16]{};
    const ssize_t readn = ::read(fd, reply, sizeof(reply));
    assert(readn > 0);
    assert(std::string_view(reply, static_cast<std::size_t>(readn)) == "ok");

    ::close(fd);
}

void waitFutureReady(std::future<void>& future,
                     std::chrono::milliseconds timeout =
                         std::chrono::milliseconds(1500)) {
    assert(future.wait_for(timeout) == std::future_status::ready);
}

void setupServerAndCallbacks(
    mini::game::SessionManager& manager,
    mini::net::TcpServer& server,
    std::string_view token,
    std::function<void(mini::game::PlayerSessionPtr)> onSession) {
    auto state = std::make_shared<ServerSessionState>();
    auto expectedToken = std::string(token);
    auto sessionCallback = std::move(onSession);

    server.setConnectionCallback([&, state](const mini::net::TcpConnectionPtr& connection) {
        if (!connection->connected()) {
            mini::net::transport::TransportSessionId transportSessionId{
                mini::net::transport::kInvalidTransportSessionId};
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                const auto it = state->connToTransport.find(connection->name());
                if (it != state->connToTransport.end()) {
                    transportSessionId = it->second;
                    state->connToTransport.erase(it);
                }
            }
            if (transportSessionId != mini::net::transport::kInvalidTransportSessionId) {
                manager.onConnectionClose(transportSessionId, "network close");
            }
            return;
        }

        const auto transportSessionId =
            state->nextTransportId.fetch_add(1, std::memory_order_relaxed);
        auto adapter = mini::net::ProtocolConnectionAdapter::createAndBind(connection);
        assert(adapter);
        adapter->setSessionId(transportSessionId);

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->connToTransport[connection->name()] = transportSessionId;
        }
    });

    server.setMessageCallback([&, state, expectedToken = std::move(expectedToken),
                               sessionCallback = std::move(sessionCallback)](
                                  const mini::net::TcpConnectionPtr& connection,
                                  mini::net::Buffer* buffer) {
        const auto incoming = buffer->retrieveAllAsString();
        if (incoming != expectedToken) {
            return;
        }

        mini::net::transport::TransportSessionId transportSessionId{
            mini::net::transport::kInvalidTransportSessionId};
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            const auto it = state->connToTransport.find(connection->name());
            if (it != state->connToTransport.end()) {
                transportSessionId = it->second;
            }
        }
        assert(transportSessionId != mini::net::transport::kInvalidTransportSessionId);

        auto session = manager.ensureSession(incoming, transportSessionId, false);
        assert(session);

        const auto state = session->state();
        if (state == mini::game::PlayerSession::State::kCreated) {
            assert(manager.markStartAuth(incoming));
            assert(manager.authenticate(incoming, 10001, "hero", "warrior"));
            assert(manager.markOnline(incoming));
        } else if (state == mini::game::PlayerSession::State::kReconnecting ||
                   state == mini::game::PlayerSession::State::kHeartbeatTimeout) {
            assert(manager.markOnline(incoming));
        }

        sessionCallback(std::move(session));

        connection->send("ok");
    });
}

void testReconnectWithinWindowReusesSession() {
    const auto port = allocateTestPort();
    mini::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();

    mini::game::SessionManager manager(logicLoop, std::chrono::milliseconds(150));

    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();
    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop,
        mini::net::InetAddress(port, true),
        "reconnect-window-reuse");
    auto* serverRaw = server.get();
    serverRaw->setThreadNum(1);

    std::promise<void> firstOnline;
    auto firstOnlineFuture = firstOnline.get_future();
    std::promise<void> secondOnline;
    auto secondOnlineFuture = secondOnline.get_future();
    std::once_flag firstOnlineOnce;
    std::once_flag secondOnlineOnce;
    std::mutex capturedMutex;
    mini::game::PlayerSessionPtr firstSession;
    mini::game::PlayerSessionPtr secondSession;

    setupServerAndCallbacks(manager,
                           *serverRaw,
                           "reconnect-window-token",
                           [&](mini::game::PlayerSessionPtr current) {
                               std::lock_guard<std::mutex> lock(capturedMutex);
                               if (!firstSession) {
                                   firstSession = current;
                                   std::call_once(firstOnlineOnce, [&] { firstOnline.set_value(); });
                                   return;
                               }
                               secondSession = current;
                               std::call_once(secondOnlineOnce, [&] {
                                   secondOnline.set_value();
                               });
                           });

    auto started = std::make_shared<std::promise<void>>();
    auto startedFuture = started->get_future();
    baseLoop->queueInLoop([serverRaw, started] {
        serverRaw->start();
        started->set_value();
    });
    waitFutureReady(startedFuture);

    runAuthClient(port, "reconnect-window-token");
    waitFutureReady(firstOnlineFuture);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    runAuthClient(port, "reconnect-window-token");
    waitFutureReady(secondOnlineFuture);

    {
        std::lock_guard<std::mutex> lock(capturedMutex);
        assert(firstSession && secondSession);
        assert(firstSession == secondSession);
    }

    auto stopped = std::make_shared<std::promise<void>>();
    auto stoppedFuture = stopped->get_future();
    baseLoop->queueInLoop([serverRaw, stopped] {
        serverRaw->stop();
        stopped->set_value();
    });
    waitFutureReady(stoppedFuture);

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
    waitFutureReady(destroyedFuture);
    logicLoop->quit();
}

void testReconnectWindowExpiresAndRecreatesSession() {
    const auto port = allocateTestPort();
    mini::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();

    mini::game::SessionManager manager(logicLoop, std::chrono::milliseconds(50));

    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();
    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop,
        mini::net::InetAddress(port, true),
        "reconnect-window-expire");
    auto* serverRaw = server.get();
    serverRaw->setThreadNum(1);

    std::promise<void> online;
    auto onlineFuture = online.get_future();
    std::promise<void> reconnected;
    auto reconnectedFuture = reconnected.get_future();
    std::promise<void> timeoutObserved;
    auto timeoutObservedFuture = timeoutObserved.get_future();
    std::once_flag onlineOnce;
    std::once_flag reconnectedOnce;
    std::once_flag timeoutObservedOnce;
    std::mutex capturedMutex;
    mini::game::PlayerSessionPtr firstSession;
    mini::game::PlayerSessionPtr secondSession;

    manager.setStateCallback([&](const std::string&,
                                 mini::game::PlayerSession::State oldState,
                                 mini::game::PlayerSession::State newState,
                                 std::string_view reason) {
        if (oldState == mini::game::PlayerSession::State::kClosing &&
            newState == mini::game::PlayerSession::State::kClosed &&
            reason == "reconnect timeout") {
            std::call_once(timeoutObservedOnce, [&] { timeoutObserved.set_value(); });
        }
    });

    setupServerAndCallbacks(manager,
                           *serverRaw,
                           "reconnect-expire-token",
                           [&](mini::game::PlayerSessionPtr current) {
                               std::lock_guard<std::mutex> lock(capturedMutex);
                               if (!firstSession) {
                                   firstSession = current;
                                   std::call_once(onlineOnce, [&] { online.set_value(); });
                                   return;
                               }
                               if (!secondSession && current != firstSession) {
                                   secondSession = current;
                                   std::call_once(reconnectedOnce, [&] { reconnected.set_value(); });
                               }
                           });

    auto started = std::make_shared<std::promise<void>>();
    auto startedFuture = started->get_future();
    baseLoop->queueInLoop([serverRaw, started] {
        serverRaw->start();
        started->set_value();
    });
    waitFutureReady(startedFuture);

    runAuthClient(port, "reconnect-expire-token");
    waitFutureReady(onlineFuture);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    waitFutureReady(timeoutObservedFuture);
    runAuthClient(port, "reconnect-expire-token");
    waitFutureReady(reconnectedFuture);

    {
        std::lock_guard<std::mutex> lock(capturedMutex);
        assert(firstSession && secondSession);
        assert(firstSession != secondSession);
    }

    auto stopped = std::make_shared<std::promise<void>>();
    auto stoppedFuture = stopped->get_future();
    baseLoop->queueInLoop([serverRaw, stopped] {
        serverRaw->stop();
        stopped->set_value();
    });
    waitFutureReady(stoppedFuture);

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
    waitFutureReady(destroyedFuture);
    logicLoop->quit();
}

}  // namespace

int main() {
    testReconnectWithinWindowReusesSession();
    testReconnectWindowExpiresAndRecreatesSession();
    return 0;
}
