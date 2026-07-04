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
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

void testMutatingOperationsMarshalSynchronouslyToLogicLoop() {
    std::promise<std::thread::id> logicThreadIdPromise;
    auto logicThreadIdFuture = logicThreadIdPromise.get_future();

    mini::net::EventLoopThread logicThread(
        [&logicThreadIdPromise](mini::net::EventLoop*) {
            logicThreadIdPromise.set_value(std::this_thread::get_id());
        });
    auto* logicLoop = logicThread.startLoop();
    const auto logicThreadId = logicThreadIdFuture.get();

    mini::game::SessionManager manager(logicLoop);
    std::atomic<int> callbackCount{0};
    std::promise<void> closedPromise;
    auto closedFuture = closedPromise.get_future();
    std::once_flag closedOnce;

    manager.setStateCallback([&](const std::string&,
                                 mini::game::PlayerSession::State,
                                 mini::game::PlayerSession::State newState,
                                 std::string_view) {
        assert(std::this_thread::get_id() == logicThreadId);
        callbackCount.fetch_add(1, std::memory_order_relaxed);
        if (newState == mini::game::PlayerSession::State::kClosing) {
            std::call_once(closedOnce, [&] { closedPromise.set_value(); });
        }
    });

    std::thread worker([&] {
        auto session = manager.ensureSession("marshal-token", 321, true);
        assert(session != nullptr);
        assert(session->state() == mini::game::PlayerSession::State::kAuthenticating);
        assert(manager.authenticate("marshal-token", 9001, "cross-thread", "rogue"));
        assert(manager.markOnline("marshal-token"));
        assert(manager.refreshHeartbeat("marshal-token"));
        assert(manager.onConnectionClose(321, "worker close"));
    });
    worker.join();

    waitFutureReady(closedFuture);
    assert(callbackCount.load(std::memory_order_relaxed) >= 4);
    auto session = manager.getSession("marshal-token");
    assert(session != nullptr);
    assert(session->state() == mini::game::PlayerSession::State::kClosing);

    logicLoop->quit();
}

void testAsyncConnectionCloseDoesNotBlockCaller() {
    std::promise<std::thread::id> logicThreadIdPromise;
    auto logicThreadIdFuture = logicThreadIdPromise.get_future();

    mini::net::EventLoopThread logicThread(
        [&logicThreadIdPromise](mini::net::EventLoop*) {
            logicThreadIdPromise.set_value(std::this_thread::get_id());
        });
    auto* logicLoop = logicThread.startLoop();
    const auto logicThreadId = logicThreadIdFuture.get();

    mini::game::SessionManager manager(logicLoop, std::chrono::milliseconds(200));
    std::promise<void> closingPromise;
    auto closingFuture = closingPromise.get_future();
    std::once_flag closingOnce;

    manager.setStateCallback([&](const std::string&,
                                 mini::game::PlayerSession::State,
                                 mini::game::PlayerSession::State newState,
                                 std::string_view) {
        assert(std::this_thread::get_id() == logicThreadId);
        if (newState == mini::game::PlayerSession::State::kClosing) {
            std::call_once(closingOnce, [&] { closingPromise.set_value(); });
        }
    });

    auto session = manager.ensureSession("async-close-token", 444, false);
    assert(session != nullptr);
    assert(manager.markStartAuth("async-close-token"));
    assert(manager.authenticate("async-close-token", 444, "async-player"));
    assert(manager.markOnline("async-close-token"));

    std::promise<void> blockerStarted;
    auto blockerStartedFuture = blockerStarted.get_future();
    std::promise<void> releaseBlockerPromise;
    auto releaseBlocker = releaseBlockerPromise.get_future().share();
    logicLoop->queueInLoop([&blockerStarted, releaseBlocker] {
        blockerStarted.set_value();
        releaseBlocker.wait();
    });
    waitFutureReady(blockerStartedFuture);

    std::promise<void> returnedPromise;
    auto returnedFuture = returnedPromise.get_future();
    std::thread worker([&] {
        manager.postConnectionClose(444, "async worker close");
        returnedPromise.set_value();
    });

    waitFutureReady(returnedFuture, std::chrono::milliseconds(100));
    assert(session->state() == mini::game::PlayerSession::State::kOnline);

    releaseBlockerPromise.set_value();
    worker.join();
    waitFutureReady(closingFuture);
    assert(session->state() == mini::game::PlayerSession::State::kClosing);

    logicLoop->quit();
}

void testAsyncHeartbeatEventuallyRunsOnLogicLoop() {
    mini::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();

    mini::game::SessionManager manager(logicLoop);
    auto session = manager.ensureSession("async-heartbeat-token", 777, false);
    assert(session != nullptr);
    assert(manager.markStartAuth("async-heartbeat-token"));
    assert(manager.authenticate("async-heartbeat-token", 777, "heartbeat-player"));
    assert(manager.markOnline("async-heartbeat-token"));

    const auto before = session->lastHeartbeatAt();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    std::thread worker([&] {
        manager.postRefreshHeartbeat("async-heartbeat-token");
    });
    worker.join();

    std::promise<void> drained;
    auto drainedFuture = drained.get_future();
    logicLoop->queueInLoop([&drained] { drained.set_value(); });
    waitFutureReady(drainedFuture);

    assert(session->lastHeartbeatAt() > before);

    logicLoop->quit();
}

void testAsyncSessionEventsBatchIntoSingleLoopFunctor() {
    mini::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();

    std::atomic<std::size_t> maxPendingFunctors{0};
    std::atomic<int> pendingDrainMetrics{0};

    std::promise<void> metricsInstalled;
    auto metricsInstalledFuture = metricsInstalled.get_future();
    logicLoop->queueInLoop([logicLoop, &metricsInstalled, &maxPendingFunctors, &pendingDrainMetrics] {
        logicLoop->setEventLoopMetricCallback(
            [&maxPendingFunctors, &pendingDrainMetrics](const mini::net::EventLoopMetricSample& sample) {
                if (sample.event != mini::net::EventLoopMetricEvent::PendingFunctorsDrained) {
                    return;
                }
                pendingDrainMetrics.fetch_add(1, std::memory_order_relaxed);
                const auto pending = sample.pendingFunctors;
                auto observed = maxPendingFunctors.load(std::memory_order_relaxed);
                while (pending > observed &&
                       !maxPendingFunctors.compare_exchange_weak(
                           observed,
                           pending,
                           std::memory_order_relaxed,
                           std::memory_order_relaxed)) {
                }
            });
        metricsInstalled.set_value();
    });
    waitFutureReady(metricsInstalledFuture);

    mini::game::SessionManager manager(logicLoop);
    constexpr int sessionCount = 16;
    std::vector<std::string> tokens;
    std::vector<mini::game::PlayerSessionPtr> sessions;
    tokens.reserve(sessionCount);
    sessions.reserve(sessionCount);

    for (int i = 0; i < sessionCount; ++i) {
        auto token = "batch-heartbeat-" + std::to_string(i);
        auto session = manager.ensureSession(token, 2000 + i, false);
        assert(session != nullptr);
        assert(manager.markStartAuth(token));
        assert(manager.authenticate(token, static_cast<std::uint64_t>(2000 + i), token));
        assert(manager.markOnline(token));
        tokens.push_back(std::move(token));
        sessions.push_back(std::move(session));
    }

    std::vector<mini::game::PlayerSession::TimePoint> before;
    before.reserve(sessions.size());
    for (const auto& session : sessions) {
        before.push_back(session->lastHeartbeatAt());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    std::promise<void> blockerStarted;
    auto blockerStartedFuture = blockerStarted.get_future();
    std::promise<void> releaseBlockerPromise;
    auto releaseBlocker = releaseBlockerPromise.get_future().share();
    logicLoop->queueInLoop([&blockerStarted, releaseBlocker] {
        blockerStarted.set_value();
        releaseBlocker.wait();
    });
    waitFutureReady(blockerStartedFuture);

    maxPendingFunctors.store(0, std::memory_order_relaxed);
    pendingDrainMetrics.store(0, std::memory_order_relaxed);

    std::thread worker([&] {
        for (const auto& token : tokens) {
            manager.postRefreshHeartbeat(token);
        }
    });
    worker.join();

    std::promise<void> drained;
    auto drainedFuture = drained.get_future();
    logicLoop->queueInLoop([&drained] { drained.set_value(); });

    releaseBlockerPromise.set_value();
    waitFutureReady(drainedFuture);

    for (std::size_t i = 0; i < sessions.size(); ++i) {
        assert(sessions[i]->lastHeartbeatAt() > before[i]);
    }
    assert(pendingDrainMetrics.load(std::memory_order_relaxed) >= 1);
    assert(maxPendingFunctors.load(std::memory_order_relaxed) == 2);

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

void testOnReconnectBindsTransportAndReportsSuccess() {
    mini::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();

    mini::game::SessionManager manager(logicLoop, std::chrono::milliseconds(80));

    auto session = manager.ensureSession("sticky-success-token", 111, false);
    assert(session != nullptr);
    assert(manager.markStartAuth("sticky-success-token"));
    assert(manager.authenticate("sticky-success-token", 3003, "hero3", "mage"));
    assert(manager.markOnline("sticky-success-token"));

    assert(manager.onConnectionClose(111, "network close"));
    assert(session->state() == mini::game::PlayerSession::State::kClosing);
    assert(manager.getSession(111) == nullptr);

    assert(manager.onReconnect("sticky-success-token", 222));
    assert(session->isReconnecting());
    assert(session->transportSessionId() == 222);
    assert(manager.getSession(111) == nullptr);
    assert(manager.getSession(222) == session);

    std::promise<void> drained;
    auto drainedFuture = drained.get_future();
    logicLoop->queueInLoop([&drained] { drained.set_value(); });
    waitFutureReady(drainedFuture);

    assert(manager.markOnline("sticky-success-token"));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    assert(manager.hasSession("sticky-success-token"));
    assert(!session->isClosed());

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
    if (shouldRun("mutations-marshal")) {
        testMutatingOperationsMarshalSynchronouslyToLogicLoop();
    }
    if (shouldRun("async-close")) {
        testAsyncConnectionCloseDoesNotBlockCaller();
    }
    if (shouldRun("async-heartbeat")) {
        testAsyncHeartbeatEventuallyRunsOnLogicLoop();
    }
    if (shouldRun("async-batch")) {
        testAsyncSessionEventsBatchIntoSingleLoopFunctor();
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
    if (shouldRun("reconnect-success")) {
        testOnReconnectBindsTransportAndReportsSuccess();
    }
    return 0;
}
