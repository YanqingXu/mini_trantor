// Task-09（PlayerSession）/Task-10（离线窗口草案）基础契约：
// - State callback 在逻辑 loop 线程触发
// - session 生命周期查找与 transport 重绑定行为
// - 弱引用由 manager 收口，移除后不再可查到

#include "mini/game/SessionManager.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string_view>
#include <thread>

namespace {

template <typename T>
void waitFutureReady(std::future<T>& future,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
    assert(future.wait_for(timeout) == std::future_status::ready);
}

void testStateCallbackMarshalsToLogicLoop() {
    std::promise<std::thread::id> logicThreadIdPromise;
    auto logicThreadIdFuture = logicThreadIdPromise.get_future();

    mini::net::EventLoopThread logicThread(
        [&logicThreadIdPromise](mini::net::EventLoop*) {
            logicThreadIdPromise.set_value(std::this_thread::get_id());
        });
    auto* logicLoop = logicThread.startLoop();
    const auto logicThreadId = logicThreadIdFuture.get();

    mini::game::SessionManager manager(logicLoop);
    std::promise<std::thread::id> callbackThreadPromise;
    auto callbackThreadFuture = callbackThreadPromise.get_future();
    std::once_flag callbackOnce;

    manager.setStateCallback(
        [&callbackThreadPromise, &callbackOnce](const std::string&,
                                                mini::game::PlayerSession::State,
                                                mini::game::PlayerSession::State,
                                                std::string_view) {
            std::call_once(callbackOnce,
                           [&] { callbackThreadPromise.set_value(std::this_thread::get_id()); });
        });

    auto session = manager.ensureSession("contract-token",
                                        mini::net::transport::kInvalidTransportSessionId,
                                        true);
    assert(session != nullptr);
    assert(session->state() == mini::game::PlayerSession::State::kAuthenticating);

    waitFutureReady(callbackThreadFuture);
    assert(callbackThreadFuture.get() == logicThreadId);

    assert(manager.authenticate("contract-token", 1001, "alice", "warrior"));
    manager.markOnline("contract-token");

    logicLoop->quit();
}

void testTransportRebindAndRemoval() {
    mini::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();

    mini::game::SessionManager manager(logicLoop);
    manager.setOwnerLoop(logicLoop);

    {
        auto session = manager.ensureSession("contract-token-2", 111, false);
        assert(session != nullptr);
        assert(session->transportSessionId() == 111);
        assert(manager.getSession(111) == session);

        assert(manager.bindTransport("contract-token-2", 222));
        assert(session->transportSessionId() == 222);
        assert(manager.getSession(111) == nullptr);
        assert(manager.getSession(222) != nullptr);

        assert(manager.hasSession("contract-token-2"));
        assert(manager.sessionCount() == 1);
    }

    // removeSession should release ownership and make weak 句柄失效
    const auto weak = manager.getSessionWeak("contract-token-2");
    assert(!weak.expired());
    assert(manager.removeSession("contract-token-2"));
    assert(!manager.hasSession("contract-token-2"));

    const auto dead = manager.getSessionWeak("contract-token-2");
    assert(!dead.lock());

    logicLoop->quit();
}

void testTransportRebindEvictsPreviousOwner() {
    mini::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();

    mini::game::SessionManager manager(logicLoop);

    auto first = manager.ensureSession("contract-token-a", 111, false);
    auto second = manager.ensureSession("contract-token-b", 222, false);
    assert(first != nullptr);
    assert(second != nullptr);
    assert(manager.getSession(222) == second);

    assert(manager.bindTransport("contract-token-a", 222));
    assert(manager.getSession(222) == first);
    assert(first->transportSessionId() == 222);
    assert(second->transportSessionId() == mini::net::transport::kInvalidTransportSessionId);
    assert(!manager.bindTransport("contract-token-a", 222));

    logicLoop->quit();
}

void testRemoveFailurePaths() {
    mini::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();

    mini::game::SessionManager manager(logicLoop);

    // 失败路径 + 幂等性（不存在会话）
    assert(!manager.removeSession("ghost-token"));
    assert(!manager.removeSessionByTransport(mini::net::transport::kInvalidTransportSessionId));

    auto session = manager.ensureSession("removable-token", 333, false);
    assert(session != nullptr);

    // 正常移除后返回 false（幂等失败）
    assert(manager.removeSession("removable-token"));
    assert(!manager.removeSession("removable-token"));
    assert(!manager.removeSessionByTransport(333));
    logicLoop->quit();
}

void testReconnectWindowKeepsSessionWithinTimeout() {
    mini::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();

    mini::game::SessionManager manager(logicLoop, std::chrono::milliseconds(100));

    auto session = manager.ensureSession("sticky-token", 555, false);
    assert(session != nullptr);

    assert(manager.markStartAuth("sticky-token"));
    assert(manager.authenticate("sticky-token", 1001, "hero", "warrior"));
    assert(manager.markOnline("sticky-token"));

    manager.onConnectionClose(555, "network close");
    assert(manager.getSession("sticky-token") == session);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(manager.hasSession("sticky-token"));

    assert(manager.markReconnecting("sticky-token"));
    assert(manager.getSession("sticky-token")->isReconnecting());
    assert(manager.markOnline("sticky-token"));
    assert(manager.hasSession("sticky-token"));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    assert(manager.hasSession("sticky-token"));
    assert(!manager.getSession("sticky-token")->isClosed());

    logicLoop->quit();
}

void testReconnectWindowExpiresAndRecreatesSession() {
    mini::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();

    mini::game::SessionManager manager(logicLoop, std::chrono::milliseconds(50));
    std::mutex mutex;
    bool seenTimeout = false;
    mini::game::PlayerSessionPtr firstSession;

    manager.setStateCallback([&](const std::string&,
                                 mini::game::PlayerSession::State oldState,
                                 mini::game::PlayerSession::State newState,
                                 std::string_view reason) {
        if (oldState == mini::game::PlayerSession::State::kClosing &&
            newState == mini::game::PlayerSession::State::kClosed &&
            reason == "reconnect timeout") {
            std::lock_guard lock(mutex);
            seenTimeout = true;
        }
    });

    firstSession = manager.ensureSession("sticky-expire-token", 888, false);
    assert(firstSession);
    assert(manager.markStartAuth("sticky-expire-token"));
    assert(manager.authenticate("sticky-expire-token", 2002, "hero2", "warrior"));
    assert(manager.markOnline("sticky-expire-token"));

    assert(manager.onConnectionClose(888, "network close"));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    assert(!manager.hasSession("sticky-expire-token"));
    assert(seenTimeout);

    auto secondSession = manager.ensureSession("sticky-expire-token", 999, false);
    assert(secondSession != nullptr);
    assert(secondSession != firstSession);
    assert(secondSession->state() == mini::game::PlayerSession::State::kCreated);

    logicLoop->quit();
}

void testOnReconnectFailureForMissingSession() {
    mini::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();

    mini::game::SessionManager manager(logicLoop);

    assert(!manager.onReconnect("ghost-token", 12345));

    logicLoop->quit();
}

}  // namespace

int main(int argc, char** argv) {
    const auto selected = argc > 1 ? std::string_view(argv[1]) : std::string_view{};

    auto shouldRun = [selected](std::string_view name) {
        return selected.empty() || selected == name;
    };

    if (shouldRun("state-callback")) {
        testStateCallbackMarshalsToLogicLoop();
    }
    if (shouldRun("rebind-removal")) {
        testTransportRebindAndRemoval();
    }
    if (shouldRun("rebind-evict")) {
        testTransportRebindEvictsPreviousOwner();
    }
    if (shouldRun("remove-failure")) {
        testRemoveFailurePaths();
    }
    if (shouldRun("reconnect-keep")) {
        testReconnectWindowKeepsSessionWithinTimeout();
    }
    if (shouldRun("reconnect-expire")) {
        testReconnectWindowExpiresAndRecreatesSession();
    }
    if (shouldRun("reconnect-missing")) {
        testOnReconnectFailureForMissingSession();
    }
    return 0;
}
