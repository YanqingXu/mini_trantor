#include "mini/game/SessionManager.h"
#include "mini/game/logic/LogicLoop.h"
#include "mini/net/EventLoopThread.h"

#include <cassert>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

template <typename T>
T waitValue(std::future<T>& future, std::chrono::milliseconds timeout) {
    assert(future.wait_for(timeout) == std::future_status::ready);
    return future.get();
}

void waitDone(std::future<void>& future, std::chrono::milliseconds timeout) {
    assert(future.wait_for(timeout) == std::future_status::ready);
}

void testEventLoopQueueMetricsRunOnOwnerThread() {
    using namespace std::chrono_literals;

    const auto callerThread = std::this_thread::get_id();
    std::promise<std::thread::id> metricThreadPromise;
    auto metricThreadFuture = metricThreadPromise.get_future();
    std::once_flag metricOnce;

    mini::net::EventLoopThread loopThread([&](mini::net::EventLoop* loop) {
        loop->setEventLoopMetricCallback([&, loop](const mini::net::EventLoopMetricSample& sample) {
            if (sample.event != mini::net::EventLoopMetricEvent::PendingFunctorsDrained) {
                return;
            }
            assert(sample.loop == loop);
            assert(sample.pendingFunctors >= 1);
            assert(sample.pendingFunctorPeak >= sample.pendingFunctors);
            assert(sample.oldestPendingLatency >= mini::net::EventLoopMetricSample::Duration::zero());
            std::call_once(metricOnce, [&] {
                metricThreadPromise.set_value(std::this_thread::get_id());
            });
        });
    });

    auto* loop = loopThread.startLoop();
    std::promise<void> ran;
    auto ranFuture = ran.get_future();
    loop->queueInLoop([&] { ran.set_value(); });

    waitDone(ranFuture, 1s);
    const auto metricThread = waitValue(metricThreadFuture, 1s);
    assert(metricThread != callerThread);

    loop->quit();
}

void testLogicLoopBacklogLagAndJitterMetrics() {
    using namespace std::chrono_literals;

    const auto callerThread = std::this_thread::get_id();
    mini::game::logic::LogicLoop logicLoop({.fixedStep = 8ms, .maxCommandsPerTick = 1});

    std::promise<std::thread::id> enqueueMetricThread;
    auto enqueueMetricFuture = enqueueMetricThread.get_future();
    std::promise<std::thread::id> tickMetricThread;
    auto tickMetricFuture = tickMetricThread.get_future();
    std::once_flag enqueueOnce;
    std::once_flag tickOnce;

    logicLoop.setMetricCallback(
        [&](const mini::game::logic::LogicLoopMetricSample& sample) {
            assert(sample.loop != nullptr);
            if (sample.event == mini::game::logic::LogicLoopMetricEvent::CommandEnqueued) {
                assert(sample.backlog >= 1);
                assert(sample.oldestLag >= std::chrono::milliseconds::zero());
                std::call_once(enqueueOnce, [&] {
                    enqueueMetricThread.set_value(std::this_thread::get_id());
                });
                return;
            }
            if (sample.event == mini::game::logic::LogicLoopMetricEvent::TickCompleted &&
                sample.drainedCommands > 0) {
                assert(sample.tickDuration >= mini::game::logic::LogicLoopMetricSample::Duration::zero());
                assert(sample.tickJitter >= mini::game::logic::LogicLoopMetricSample::Duration::zero());
                std::call_once(tickOnce, [&] {
                    tickMetricThread.set_value(std::this_thread::get_id());
                });
            }
        });

    logicLoop.setProcessor(
        [](const mini::game::logic::GameCommand&,
           std::vector<mini::game::logic::GameCommand>&) {});

    logicLoop.start();
    assert(logicLoop.submit("session", {}, "payload"));

    const auto enqueueThread = waitValue(enqueueMetricFuture, 1s);
    const auto tickThread = waitValue(tickMetricFuture, 1s);
    assert(enqueueThread != callerThread);
    assert(tickThread != callerThread);

    logicLoop.stop();
}

void testSessionReconnectMetrics() {
    using namespace std::chrono_literals;

    const auto callerThread = std::this_thread::get_id();
    mini::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();

    mini::game::SessionManager manager(logicLoop, 200ms);
    std::promise<std::thread::id> startedMetricThread;
    auto startedMetricFuture = startedMetricThread.get_future();
    std::promise<std::thread::id> successMetricThread;
    auto successMetricFuture = successMetricThread.get_future();
    std::once_flag startedOnce;
    std::once_flag successOnce;

    manager.setMetricCallback([&](const mini::game::SessionMetricSample& sample) {
        assert(sample.sessionToken == "token-1");
        if (sample.event == mini::game::SessionMetricEvent::ReconnectWindowStarted) {
            std::call_once(startedOnce, [&] {
                startedMetricThread.set_value(std::this_thread::get_id());
            });
            return;
        }
        if (sample.event == mini::game::SessionMetricEvent::ReconnectSucceeded) {
            assert(sample.success);
            assert(sample.reconnectDuration >= mini::game::SessionMetricSample::Duration::zero());
            std::call_once(successOnce, [&] {
                successMetricThread.set_value(std::this_thread::get_id());
            });
        }
    });

    auto session = manager.ensureSession("token-1", 1);
    assert(session);
    assert(manager.authenticate("token-1", 7, "player"));
    assert(manager.markOnline("token-1"));
    assert(manager.onConnectionClose(1, "network drop"));

    const auto startedThread = waitValue(startedMetricFuture, 1s);
    assert(startedThread != callerThread);

    assert(manager.bindTransport("token-1", 2));
    const auto successThread = waitValue(successMetricFuture, 1s);
    assert(successThread != callerThread);

    logicLoop->quit();
}

void testSessionAsyncEventDrainMetrics() {
    using namespace std::chrono_literals;

    const auto callerThread = std::this_thread::get_id();
    mini::net::EventLoopThread logicThread;
    auto* logicLoop = logicThread.startLoop();

    mini::game::SessionManager manager(logicLoop);
    constexpr int sessionCount = 8;
    std::vector<std::string> tokens;
    tokens.reserve(sessionCount);

    for (int i = 0; i < sessionCount; ++i) {
        auto token = "async-metric-token-" + std::to_string(i);
        auto session = manager.ensureSession(token, 8000 + i, false);
        assert(session != nullptr);
        assert(manager.markStartAuth(token));
        assert(manager.authenticate(token, static_cast<std::uint64_t>(8000 + i), token));
        assert(manager.markOnline(token));
        tokens.push_back(std::move(token));
    }

    std::promise<std::thread::id> metricThreadPromise;
    auto metricThreadFuture = metricThreadPromise.get_future();
    std::once_flag metricOnce;

    manager.setMetricCallback([&](const mini::game::SessionMetricSample& sample) {
        if (sample.event != mini::game::SessionMetricEvent::AsyncEventsDrained) {
            return;
        }
        assert(sample.loop == logicLoop);
        assert(sample.pendingEvents == static_cast<std::size_t>(sessionCount));
        assert(sample.drainedEvents == static_cast<std::size_t>(sessionCount));
        assert(sample.oldestEventLag >= mini::game::SessionMetricSample::Duration::zero());
        std::call_once(metricOnce, [&] {
            metricThreadPromise.set_value(std::this_thread::get_id());
        });
    });

    std::promise<void> blockerStarted;
    auto blockerStartedFuture = blockerStarted.get_future();
    std::promise<void> releaseBlockerPromise;
    auto releaseBlocker = releaseBlockerPromise.get_future().share();
    logicLoop->queueInLoop([&blockerStarted, releaseBlocker] {
        blockerStarted.set_value();
        releaseBlocker.wait();
    });
    waitDone(blockerStartedFuture, 1s);

    std::thread worker([&] {
        for (const auto& token : tokens) {
            manager.postRefreshHeartbeat(token);
        }
    });
    worker.join();

    releaseBlockerPromise.set_value();
    const auto metricThread = waitValue(metricThreadFuture, 1s);
    assert(metricThread != callerThread);

    logicLoop->quit();
}

void testGamePipelineMetricsSchema() {
    bool inputObserved = false;
    mini::game::GamePipelineMetricCallback callback =
        [&](const mini::game::GamePipelineMetricSample& sample) {
            inputObserved = true;
            assert(sample.event == mini::game::GamePipelineMetricEvent::InputBatchProcessed);
            assert(sample.framesDecoded == 16);
            assert(sample.bytesConsumed == 1024);
            assert(sample.bufferedBytes == 128);
            assert(sample.continuationScheduled);
            assert(sample.batchDuration >= mini::game::GamePipelineMetricSample::Duration::zero());
        };

    mini::game::GamePipelineMetricSample sample;
    sample.event = mini::game::GamePipelineMetricEvent::InputBatchProcessed;
    sample.framesDecoded = 16;
    sample.bytesConsumed = 1024;
    sample.bufferedBytes = 128;
    sample.continuationScheduled = true;
    sample.batchDuration = std::chrono::milliseconds(1);

    callback(sample);
    assert(inputObserved);
}

}  // namespace

int main() {
    testEventLoopQueueMetricsRunOnOwnerThread();
    testLogicLoopBacklogLagAndJitterMetrics();
    testSessionReconnectMetrics();
    testSessionAsyncEventDrainMetrics();
    testGamePipelineMetricsSchema();
    return 0;
}
