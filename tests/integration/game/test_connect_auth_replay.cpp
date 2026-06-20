// 集成级验证：TCP 连接驱动 SessionManager，包含首次登录与同 token 重连。
//
// - 客户端 A 首次连接发送 token，触发 create/auth/online
// - 连接断开后状态变更为 closing
// - 客户端 B 用同 token 重连后，SessionManager 复用同一 session 并完成 online 迁移

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
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <unistd.h>

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
                         std::chrono::milliseconds(2000)) {
    assert(future.wait_for(timeout) == std::future_status::ready);
}

}  // namespace

int main() {
    const uint16_t port = allocateTestPort();

    std::promise<std::thread::id> logicThreadIdPromise;
    auto logicThreadIdFuture = logicThreadIdPromise.get_future();

    mini::net::EventLoopThread logicThread(
        [&logicThreadIdPromise](mini::net::EventLoop*) {
            logicThreadIdPromise.set_value(std::this_thread::get_id());
        });
    auto* logicLoop = logicThread.startLoop();
    auto logicThreadId = logicThreadIdFuture.get();

    mini::game::SessionManager manager(logicLoop);
    std::promise<void> firstOnline;
    std::promise<void> replayOnline;
    std::promise<void> sessionClosing;

    auto firstOnlineFuture = firstOnline.get_future();
    auto replayOnlineFuture = replayOnline.get_future();
    auto sessionClosingFuture = sessionClosing.get_future();

    std::atomic<int> onlineCount{0};
    std::mutex captureMutex;
    bool sessionCaptured = false;
    mini::game::PlayerSessionPtr capturedSession;
    std::once_flag sessionClosingOnce;

    manager.setStateCallback(
        [&onlineCount,
         &firstOnline,
         &replayOnline,
         &sessionClosing,
         &sessionClosingOnce,
         &logicThreadId](const std::string& token,
                          mini::game::PlayerSession::State oldState,
                          mini::game::PlayerSession::State newState,
                          std::string_view reason) {
            (void)oldState;
            (void)reason;
            assert(std::this_thread::get_id() == logicThreadId);
            if (token != "replay-player") {
                return;
            }
            if (newState == mini::game::PlayerSession::State::kOnline) {
                const int index = ++onlineCount;
                if (index == 1) {
                    firstOnline.set_value();
                } else if (index == 2) {
                    replayOnline.set_value();
                }
            }
            if (newState == mini::game::PlayerSession::State::kClosing) {
                std::call_once(sessionClosingOnce, [&] { sessionClosing.set_value(); });
            }
        });

    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();

    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop, mini::net::InetAddress(port, true), "game-connect-auth-replay");
    auto* serverRaw = server.get();
    serverRaw->setThreadNum(1);

    std::atomic<mini::net::transport::TransportSessionId> nextTransportId{
        mini::net::transport::kFirstTransportSessionId};
    std::unordered_map<std::string, mini::net::transport::TransportSessionId> connToTransport;
    std::mutex connMapMutex;

    serverRaw->setConnectionCallback([&](const mini::net::TcpConnectionPtr& connection) {
        if (!connection->connected()) {
            mini::net::transport::TransportSessionId transportSessionId =
                mini::net::transport::kInvalidTransportSessionId;
            {
                std::lock_guard lock(connMapMutex);
                const auto it = connToTransport.find(connection->name());
                if (it != connToTransport.end()) {
                    transportSessionId = it->second;
                    connToTransport.erase(it);
                }
            }
            if (transportSessionId != mini::net::transport::kInvalidTransportSessionId) {
                manager.onConnectionClose(transportSessionId, "network close");
            }
            return;
        }

        const auto transportSessionId =
            nextTransportId.fetch_add(1, std::memory_order_relaxed);

        auto adapter = mini::net::ProtocolConnectionAdapter::createAndBind(connection);
        assert(adapter);
        adapter->setSessionId(transportSessionId);

        {
            std::lock_guard lock(connMapMutex);
            connToTransport[connection->name()] = transportSessionId;
        }
    });

    serverRaw->setMessageCallback(
        [&](const mini::net::TcpConnectionPtr& connection, mini::net::Buffer* buffer) {
            const std::string token = buffer->retrieveAllAsString();
            if (token.empty()) {
                return;
            }

            mini::net::transport::TransportSessionId transportSessionId =
                mini::net::transport::kInvalidTransportSessionId;
            {
                std::lock_guard lock(connMapMutex);
                const auto it = connToTransport.find(connection->name());
                assert(it != connToTransport.end());
                transportSessionId = it->second;
            }

            auto* adapter = mini::net::ProtocolConnectionAdapter::getFrom(connection);
            assert(adapter);
            assert(adapter->sessionId() == transportSessionId);

            auto session = manager.ensureSession(token, transportSessionId, false);
            assert(session);

            const auto state = session->state();
            if (state == mini::game::PlayerSession::State::kClosing) {
                manager.markReconnecting(token);
            } else if (state == mini::game::PlayerSession::State::kCreated) {
                manager.markStartAuth(token);
                manager.authenticate(token, 10001, "hero", "warrior");
            } else if (state == mini::game::PlayerSession::State::kAuthenticating) {
                manager.authenticate(token, 10001, "hero", "warrior");
            }
            assert(manager.markOnline(token));

            {
                std::lock_guard lock(captureMutex);
                if (!sessionCaptured) {
                    sessionCaptured = true;
                    capturedSession = session;
                }
                assert(session == capturedSession);
            }

            connection->send("ok");
        });

    auto started = std::make_shared<std::promise<void>>();
    auto startedFuture = started->get_future();
    baseLoop->queueInLoop([serverRaw, started] {
        serverRaw->start();
        started->set_value();
    });
    assert(startedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    runAuthClient(port, "replay-player");
    waitFutureReady(firstOnlineFuture);
    waitFutureReady(sessionClosingFuture);
    runAuthClient(port, "replay-player");
    waitFutureReady(replayOnlineFuture);

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

    logicLoop->quit();
    mini::game::PlayerSessionPtr captured;
    {
        std::lock_guard lock(captureMutex);
        captured = capturedSession;
    }
    assert(captured);
    assert(captured->sessionId() == "replay-player");
    const auto managed = manager.getSession("replay-player");
    assert(managed == captured);
    assert(managed && !managed->isClosed());

    return 0;
}
