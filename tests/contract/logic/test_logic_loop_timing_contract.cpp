#include "mini/game/logic/LogicLoop.h"

#include <cassert>
#include <any>
#include <chrono>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

void waitFutureReady(std::future<void>& future, std::chrono::milliseconds timeout) {
    assert(future.wait_for(timeout) == std::future_status::ready);
}

void waitUntil(std::function<bool()> predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(predicate());
}

class FakeEndpoint : public mini::net::transport::ITransportEndpoint {
public:
    FakeEndpoint(mini::net::EventLoop* loop,
                 mini::net::transport::TransportSessionId sessionId,
                 std::promise<void>& sentPromise)
        : loop_(loop),
          sessionId_(sessionId),
          sentPromise_(sentPromise) {}

    void send(std::string_view data) override {
        std::scoped_lock lock(mutex_);
        sentPayloads_.emplace_back(data);
        std::call_once(sentOnce_, [&] { sentPromise_.set_value(); });
    }

    void shutdown() override {}
    void forceClose() override {}
    bool connected() const noexcept override { return true; }
    mini::net::EventLoop* getLoop() const noexcept override { return loop_; }
    std::string_view name() const noexcept override { return name_; }

    mini::net::transport::TransportSessionId sessionId() const noexcept override {
        return sessionId_;
    }

    void setSessionId(mini::net::transport::TransportSessionId id) override {
        sessionId_ = id;
    }

    mini::net::transport::TransportKind transportKind() const noexcept override {
        return mini::net::transport::TransportKind::kTcp;
    }

    void setTransportContext(std::any ctx) override {
        context_ = std::move(ctx);
    }

    const std::any& getTransportContext() const noexcept override {
        return context_;
    }

    std::any& getTransportContext() noexcept override {
        return context_;
    }

    std::vector<std::string> sentPayloads() const {
        std::scoped_lock lock(mutex_);
        return sentPayloads_;
    }

private:
    mini::net::EventLoop* loop_{nullptr};
    mini::net::transport::TransportSessionId sessionId_{
        mini::net::transport::kInvalidTransportSessionId};
    std::promise<void>& sentPromise_;
    std::string name_{"fake-endpoint"};
    std::any context_;
    mutable std::mutex mutex_;
    std::once_flag sentOnce_;
    std::vector<std::string> sentPayloads_;
};

void testProcessorRunsOnLogicThread() {
    using namespace std::chrono_literals;

    mini::game::logic::LogicLoop logicLoop({.fixedStep = 12ms, .maxCommandsPerTick = 16});
    std::promise<std::thread::id> logicThreadPromise;
    auto logicThreadFuture = logicThreadPromise.get_future();
    std::promise<void> processDone;
    auto processDoneFuture = processDone.get_future();
    std::once_flag seenOnce;

    logicLoop.setProcessor([&](const mini::game::logic::GameCommand&,
                               std::vector<mini::game::logic::GameCommand>&) {
        std::call_once(seenOnce, [&] { logicThreadPromise.set_value(std::this_thread::get_id()); });
        processDone.set_value();
    });

    logicLoop.start();
    const bool accepted = logicLoop.submit("session", {}, "msg-1");
    assert(accepted);

    waitFutureReady(processDoneFuture, 500ms);
    const auto logicThreadId = logicThreadFuture.get();
    assert(logicThreadId != std::this_thread::get_id());
    waitUntil([&] { return logicLoop.processedCount() == 1; }, 500ms);
    assert(logicLoop.processedCount() == 1);
    assert(logicLoop.backlog() == 0);

    logicLoop.stop();
}

void testSubmitWithResultReportsLifecycleState() {
    using namespace std::chrono_literals;

    mini::game::logic::LogicLoop logicLoop({.fixedStep = 50ms, .maxCommandsPerTick = 4});

    auto beforeStart = logicLoop.submitWithResult(
        "before-start",
        std::weak_ptr<mini::net::TcpConnection>{},
        "payload");
    assert(!beforeStart.accepted);
    assert(beforeStart.action == mini::game::GameBackpressureAction::Reject);
    assert(beforeStart.reason == mini::game::GameBackpressureReason::LogicLoopNotRunning);
    assert(logicLoop.backlog() == 0);

    logicLoop.start();
    auto running = logicLoop.submitWithResult(
        "running",
        std::weak_ptr<mini::net::TcpConnection>{},
        "payload");
    assert(running.accepted);
    assert(running.action == mini::game::GameBackpressureAction::Accept);

    logicLoop.stop();
    auto afterStop = logicLoop.submitWithResult(
        "after-stop",
        std::weak_ptr<mini::net::TcpConnection>{},
        "payload");
    assert(!afterStop.accepted);
    assert(afterStop.action == mini::game::GameBackpressureAction::Reject);
    assert(afterStop.reason == mini::game::GameBackpressureReason::LogicLoopNotRunning);
    assert(logicLoop.backlog() == 0);
}

void testConcurrentSubmittersStillProcessOnSingleLogicThread() {
    using namespace std::chrono_literals;

    constexpr int kProducerThreads = 4;
    constexpr int kCommandsPerThread = 24;
    constexpr int kTotalCommands = kProducerThreads * kCommandsPerThread;

    mini::game::logic::LogicLoop logicLoop({.fixedStep = 4ms, .maxCommandsPerTick = 64});
    std::atomic<int> processed{0};
    std::mutex observedMutex;
    std::set<std::thread::id> processorThreads;
    std::set<std::thread::id> producerThreads;

    logicLoop.setProcessor([&](const mini::game::logic::GameCommand&,
                               std::vector<mini::game::logic::GameCommand>&) {
        {
            std::scoped_lock lock(observedMutex);
            processorThreads.insert(std::this_thread::get_id());
        }
        ++processed;
    });

    logicLoop.start();

    std::vector<std::thread> producers;
    producers.reserve(kProducerThreads);
    for (int i = 0; i < kProducerThreads; ++i) {
        producers.emplace_back([&, i] {
            {
                std::scoped_lock lock(observedMutex);
                producerThreads.insert(std::this_thread::get_id());
            }
            for (int j = 0; j < kCommandsPerThread; ++j) {
                auto result = logicLoop.submitWithResult(
                    "producer-" + std::to_string(i),
                    std::weak_ptr<mini::net::TcpConnection>{},
                    "cmd-" + std::to_string(j));
                assert(result.accepted);
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    waitUntil([&] { return processed.load() == kTotalCommands; }, 2s);
    {
        std::scoped_lock lock(observedMutex);
        assert(processorThreads.size() == 1);
        assert(producerThreads.size() == kProducerThreads);
        assert(producerThreads.find(*processorThreads.begin()) == producerThreads.end());
    }
    assert(logicLoop.backlog() == 0);

    logicLoop.stop();
}

void testDefaultOutputDispatchMetrics() {
    using namespace std::chrono_literals;

    const auto callerThread = std::this_thread::get_id();
    std::promise<std::thread::id> targetThreadIdPromise;
    auto targetThreadIdFuture = targetThreadIdPromise.get_future();
    mini::net::EventLoopThread targetThread(
        [&targetThreadIdPromise](mini::net::EventLoop*) {
            targetThreadIdPromise.set_value(std::this_thread::get_id());
        });
    auto* targetLoop = targetThread.startLoop();
    const auto targetThreadId = targetThreadIdFuture.get();

    std::promise<void> sentPromise;
    auto sentFuture = sentPromise.get_future();
    auto endpoint = std::make_shared<FakeEndpoint>(targetLoop, 9001, sentPromise);

    mini::game::logic::LogicLoop logicLoop({.fixedStep = 8ms, .maxCommandsPerTick = 8});
    std::promise<std::thread::id> dispatchedMetricThreadPromise;
    auto dispatchedMetricThreadFuture = dispatchedMetricThreadPromise.get_future();
    std::promise<std::thread::id> sentMetricThreadPromise;
    auto sentMetricThreadFuture = sentMetricThreadPromise.get_future();
    std::once_flag dispatchedOnce;
    std::once_flag sentMetricOnce;

    logicLoop.setMetricCallback(
        [&](const mini::game::logic::LogicLoopMetricSample& sample) {
            if (sample.event == mini::game::logic::LogicLoopMetricEvent::OutputDispatched) {
                assert(sample.outputBatch == 1);
                assert(sample.queuedOutputs == 1);
                assert(sample.droppedOutputs == 0);
                assert(sample.outputBytes == std::string_view("logic-out").size());
                std::call_once(dispatchedOnce, [&] {
                    dispatchedMetricThreadPromise.set_value(std::this_thread::get_id());
                });
                return;
            }

            if (sample.event == mini::game::logic::LogicLoopMetricEvent::OutputSent) {
                assert(sample.loop == targetLoop);
                assert(sample.outputBatch == 1);
                assert(sample.queuedOutputs == 1);
                assert(sample.outputBytes == std::string_view("logic-out").size());
                assert(sample.outputQueueLatency >=
                       mini::game::logic::LogicLoopMetricSample::Duration::zero());
                std::call_once(sentMetricOnce, [&] {
                    sentMetricThreadPromise.set_value(std::this_thread::get_id());
                });
            }
        });

    logicLoop.setProcessor(
        [endpoint](const mini::game::logic::GameCommand& command,
                   std::vector<mini::game::logic::GameCommand>& outputs) {
            outputs.emplace_back(command.sessionId,
                                 command.transportSessionId,
                                 endpoint,
                                 "logic-out");
        });

    logicLoop.start();
    assert(logicLoop.submit("output-session", 9001, endpoint, "logic-in"));

    waitFutureReady(sentFuture, 1s);
    assert(dispatchedMetricThreadFuture.wait_for(1s) == std::future_status::ready);
    assert(sentMetricThreadFuture.wait_for(1s) == std::future_status::ready);
    assert(dispatchedMetricThreadFuture.get() != callerThread);
    assert(sentMetricThreadFuture.get() == targetThreadId);

    const auto sentPayloads = endpoint->sentPayloads();
    assert(sentPayloads.size() == 1);
    assert(sentPayloads.front() == "logic-out");

    logicLoop.stop();
    targetLoop->quit();
}

void testDefaultOutputDropsPayloadOverHardLimit() {
    using namespace std::chrono_literals;

    std::promise<void> targetStartedPromise;
    auto targetStartedFuture = targetStartedPromise.get_future();
    mini::net::EventLoopThread targetThread(
        [&targetStartedPromise](mini::net::EventLoop*) {
            targetStartedPromise.set_value();
        });
    auto* targetLoop = targetThread.startLoop();
    assert(targetStartedFuture.wait_for(1s) == std::future_status::ready);

    std::promise<void> sentPromise;
    auto sentFuture = sentPromise.get_future();
    auto endpoint = std::make_shared<FakeEndpoint>(targetLoop, 9002, sentPromise);

    mini::game::logic::LogicLoop::Options options;
    options.fixedStep = 8ms;
    options.maxCommandsPerTick = 8;
    options.output.hardQueuedBytes = 4;
    mini::game::logic::LogicLoop logicLoop(options);

    std::promise<void> droppedMetricPromise;
    auto droppedMetricFuture = droppedMetricPromise.get_future();
    std::once_flag droppedOnce;
    logicLoop.setBackpressureMetricCallback(
        [&](const mini::game::GameBackpressureMetricSample& sample) {
            if (sample.event != mini::game::GameBackpressureMetricEvent::OutputDropped) {
                return;
            }
            assert(sample.layer == mini::game::GameBackpressureLayer::OutputSend);
            assert(sample.action == mini::game::GameBackpressureAction::Reject);
            assert(sample.reason == mini::game::GameBackpressureReason::OutputQueuedBytesHardLimit);
            assert(sample.sessionToken == "output-drop");
            assert(sample.transportSessionId == 9002);
            assert(sample.payloadBytes == std::string_view("too-large").size());
            assert(sample.currentValue == std::string_view("too-large").size());
            assert(sample.hardLimit == 4);
            std::call_once(droppedOnce, [&] { droppedMetricPromise.set_value(); });
        });

    std::promise<void> dispatchedMetricPromise;
    auto dispatchedMetricFuture = dispatchedMetricPromise.get_future();
    std::once_flag dispatchedOnce;
    logicLoop.setMetricCallback(
        [&](const mini::game::logic::LogicLoopMetricSample& sample) {
            if (sample.event != mini::game::logic::LogicLoopMetricEvent::OutputDispatched) {
                return;
            }
            assert(sample.outputBatch == 1);
            assert(sample.queuedOutputs == 0);
            assert(sample.droppedOutputs == 1);
            assert(sample.outputBytes == 0);
            std::call_once(dispatchedOnce, [&] { dispatchedMetricPromise.set_value(); });
        });

    logicLoop.setProcessor(
        [endpoint](const mini::game::logic::GameCommand& command,
                   std::vector<mini::game::logic::GameCommand>& outputs) {
            outputs.emplace_back(command.sessionId,
                                 command.transportSessionId,
                                 endpoint,
                                 "too-large");
        });

    logicLoop.start();
    assert(logicLoop.submit("output-drop", 9002, endpoint, "logic-in"));

    waitFutureReady(droppedMetricFuture, 1s);
    waitFutureReady(dispatchedMetricFuture, 1s);
    assert(sentFuture.wait_for(100ms) == std::future_status::timeout);
    assert(endpoint->sentPayloads().empty());

    logicLoop.stop();
    targetLoop->quit();
}

void testDefaultOutputDropsOnlyLowPriorityAtSoftLimit() {
    using namespace std::chrono_literals;

    std::promise<void> targetStartedPromise;
    auto targetStartedFuture = targetStartedPromise.get_future();
    mini::net::EventLoopThread targetThread(
        [&targetStartedPromise](mini::net::EventLoop*) {
            targetStartedPromise.set_value();
        });
    auto* targetLoop = targetThread.startLoop();
    assert(targetStartedFuture.wait_for(1s) == std::future_status::ready);

    std::promise<void> sentPromise;
    auto sentFuture = sentPromise.get_future();
    auto endpoint = std::make_shared<FakeEndpoint>(targetLoop, 9003, sentPromise);

    mini::game::logic::LogicLoop::Options options;
    options.fixedStep = 50ms;
    options.maxCommandsPerTick = 8;
    options.output.softQueuedBytes = 4;
    options.output.hardQueuedBytes = 16;
    options.output.priority.softLimitMinPriority = mini::game::GameMessagePriority::Normal;
    mini::game::logic::LogicLoop logicLoop(options);

    std::promise<void> droppedMetricPromise;
    auto droppedMetricFuture = droppedMetricPromise.get_future();
    std::once_flag droppedOnce;
    logicLoop.setBackpressureMetricCallback(
        [&](const mini::game::GameBackpressureMetricSample& sample) {
            if (sample.event != mini::game::GameBackpressureMetricEvent::OutputDropped ||
                sample.action != mini::game::GameBackpressureAction::DropLowPriority) {
                return;
            }
            assert(sample.layer == mini::game::GameBackpressureLayer::OutputSend);
            assert(sample.reason == mini::game::GameBackpressureReason::OutputQueuedBytesSoftLimit);
            assert(sample.sessionToken == "output-priority");
            assert(sample.priority == mini::game::toMetricPriority(mini::game::GameMessagePriority::Low));
            assert(sample.payloadBytes == std::string_view("low-load").size());
            assert(sample.currentValue == std::string_view("low-load").size());
            assert(sample.softLimit == 4);
            assert(sample.hardLimit == 16);
            std::call_once(droppedOnce, [&] { droppedMetricPromise.set_value(); });
        });

    std::promise<void> dispatchedMetricPromise;
    auto dispatchedMetricFuture = dispatchedMetricPromise.get_future();
    std::once_flag dispatchedOnce;
    logicLoop.setMetricCallback(
        [&](const mini::game::logic::LogicLoopMetricSample& sample) {
            if (sample.event != mini::game::logic::LogicLoopMetricEvent::OutputDispatched) {
                return;
            }
            if (sample.outputBatch == 2) {
                assert(sample.queuedOutputs == 1);
                assert(sample.droppedOutputs == 1);
                std::call_once(dispatchedOnce, [&] { dispatchedMetricPromise.set_value(); });
            }
        });

    logicLoop.setProcessor(
        [endpoint](const mini::game::logic::GameCommand& command,
                   std::vector<mini::game::logic::GameCommand>& outputs) {
            const auto payload = command.payload == "low-in" ? "low-load" : "high-load";
            outputs.emplace_back(command.sessionId,
                                 command.transportSessionId,
                                 endpoint,
                                 payload,
                                 command.priority);
        });

    logicLoop.start();
    assert(logicLoop.submit(
        "output-priority",
        9003,
        endpoint,
        "low-in",
        mini::game::toMetricPriority(mini::game::GameMessagePriority::Low)));
    assert(logicLoop.submit(
        "output-priority",
        9003,
        endpoint,
        "high-in",
        mini::game::toMetricPriority(mini::game::GameMessagePriority::High)));

    waitFutureReady(droppedMetricFuture, 1s);
    waitFutureReady(dispatchedMetricFuture, 1s);
    waitFutureReady(sentFuture, 1s);

    const auto sentPayloads = endpoint->sentPayloads();
    assert(sentPayloads.size() == 1);
    assert(sentPayloads.front() == "high-load");

    logicLoop.stop();
    targetLoop->quit();
}

void testLifecycleStartAndStopAreSafe() {
    mini::game::logic::LogicLoop logicLoop;

    logicLoop.start();
    logicLoop.start(); // idempotent no-op

    logicLoop.stop();
    logicLoop.stop(); // idempotent no-op

    logicLoop.start();
    logicLoop.stop();
}

void testTickBacklogAndLagContract() {
    using namespace std::chrono_literals;

    mini::game::logic::LogicLoop logicLoop({.fixedStep = 12ms, .maxCommandsPerTick = 1});
    std::atomic<int> processed{0};
    logicLoop.setProcessor(
        [&processed](const mini::game::logic::GameCommand&, std::vector<mini::game::logic::GameCommand>&) {
            ++processed;
        });

    logicLoop.start();
    const auto initialLag = logicLoop.oldestLag();
    assert(initialLag == std::chrono::milliseconds::zero());

    logicLoop.submit("batch", {}, "a");
    logicLoop.submit("batch", {}, "b");
    logicLoop.submit("batch", {}, "c");

    const auto backlogAfterSubmit = logicLoop.backlog();
    assert(backlogAfterSubmit >= 2);

    std::this_thread::sleep_for(20ms);
    assert(logicLoop.backlog() >= 1);
    assert(processed.load() > 0);

    std::this_thread::sleep_for(80ms);
    assert(logicLoop.backlog() == 0);
    assert(processed.load() == 3);
    logicLoop.stop();
}

void testStopPreventsProcessingAfterShutdown() {
    mini::game::logic::LogicLoop logicLoop(mini::game::logic::LogicLoop::Options{
        std::chrono::milliseconds(8),
        10});
    std::atomic<int> processed{0};

    logicLoop.setProcessor(
        [&processed](const mini::game::logic::GameCommand&, std::vector<mini::game::logic::GameCommand>&) {
            ++processed;
        });

    logicLoop.start();
    logicLoop.stop();
    const bool accepted = logicLoop.submit("late", {}, "should_not_process");
    assert(!accepted);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(processed.load() == 0);
}

void testSubmitRejectedAfterStop() {
    mini::game::logic::LogicLoop logicLoop;

    logicLoop.start();
    logicLoop.stop();
    assert(!logicLoop.submit("late", {}, "should_not_enqueue"));
    assert(logicLoop.backlog() == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(logicLoop.processedCount() == 0);
}

void testLogicAdmissionRejectsHardBacklogAndEmitsMetric() {
    using namespace std::chrono_literals;

    mini::game::logic::LogicLoop::Options options;
    options.fixedStep = 50ms;
    options.maxCommandsPerTick = 1;
    options.admission.hardBacklog = 1;
    mini::game::logic::LogicLoop logicLoop(options);

    std::atomic<int> processed{0};
    logicLoop.setProcessor(
        [&processed](const mini::game::logic::GameCommand&,
                     std::vector<mini::game::logic::GameCommand>&) {
            ++processed;
        });

    std::promise<void> rejectedMetricPromise;
    auto rejectedMetricFuture = rejectedMetricPromise.get_future();
    std::once_flag rejectedOnce;
    logicLoop.setBackpressureMetricCallback(
        [&](const mini::game::GameBackpressureMetricSample& sample) {
            if (sample.event != mini::game::GameBackpressureMetricEvent::LogicRejected) {
                return;
            }
            assert(sample.layer == mini::game::GameBackpressureLayer::LogicAdmission);
            assert(sample.action == mini::game::GameBackpressureAction::Reject);
            assert(sample.reason == mini::game::GameBackpressureReason::LogicBacklogHardLimit);
            assert(sample.sessionToken == "overflow");
            assert(sample.backlog == 1);
            assert(sample.currentValue == 1);
            assert(sample.hardLimit == 1);
            std::call_once(rejectedOnce, [&] { rejectedMetricPromise.set_value(); });
        });

    logicLoop.start();
    auto first = logicLoop.submitWithResult(
        "accepted",
        std::weak_ptr<mini::net::TcpConnection>{},
        "first");
    assert(first.accepted);

    auto second = logicLoop.submitWithResult(
        "overflow",
        std::weak_ptr<mini::net::TcpConnection>{},
        "second");
    assert(!second.accepted);
    assert(second.reason == mini::game::GameBackpressureReason::LogicBacklogHardLimit);
    assert(second.backlog == 1);

    waitFutureReady(rejectedMetricFuture, 1s);
    std::this_thread::sleep_for(120ms);
    assert(processed.load() == 1);
    logicLoop.stop();
}

void testLogicAdmissionRejectsOldestLagAndDoesNotProcessRejectedCommand() {
    using namespace std::chrono_literals;

    mini::game::logic::LogicLoop::Options options;
    options.fixedStep = 1s;
    options.maxCommandsPerTick = 1;
    options.admission.hardBacklog = 4;
    options.admission.hardOldestLag = 20ms;
    mini::game::logic::LogicLoop logicLoop(options);

    std::atomic<int> processed{0};
    logicLoop.setProcessor(
        [&processed](const mini::game::logic::GameCommand&,
                     std::vector<mini::game::logic::GameCommand>&) {
            ++processed;
        });

    std::promise<void> rejectedMetricPromise;
    auto rejectedMetricFuture = rejectedMetricPromise.get_future();
    std::once_flag rejectedOnce;
    logicLoop.setBackpressureMetricCallback(
        [&](const mini::game::GameBackpressureMetricSample& sample) {
            if (sample.event != mini::game::GameBackpressureMetricEvent::LogicRejected) {
                return;
            }
            assert(sample.reason == mini::game::GameBackpressureReason::LogicOldestLagHardLimit);
            assert(sample.sessionToken == "lagged");
            assert(sample.currentValue >= 20);
            assert(sample.hardLimit == 20);
            assert(sample.queueLatency >= 20ms);
            std::call_once(rejectedOnce, [&] { rejectedMetricPromise.set_value(); });
        });

    logicLoop.start();
    auto first = logicLoop.submitWithResult(
        "stalled",
        std::weak_ptr<mini::net::TcpConnection>{},
        "first");
    assert(first.accepted);
    std::this_thread::sleep_for(35ms);

    auto second = logicLoop.submitWithResult(
        "lagged",
        std::weak_ptr<mini::net::TcpConnection>{},
        "second");
    assert(!second.accepted);
    assert(second.reason == mini::game::GameBackpressureReason::LogicOldestLagHardLimit);

    waitFutureReady(rejectedMetricFuture, 1s);
    assert(logicLoop.backlog() == 1);
    assert(processed.load() == 0);
    logicLoop.stop();
}

void testDestructorStopsRunningLoop() {
    auto logicLoop = std::make_unique<mini::game::logic::LogicLoop>(
        mini::game::logic::LogicLoop::Options{
            std::chrono::milliseconds(8),
            10});
    std::atomic<int> processed{0};

    logicLoop->setProcessor(
        [&processed](const mini::game::logic::GameCommand&, std::vector<mini::game::logic::GameCommand>&) {
            ++processed;
        });
    logicLoop->start();
    assert(logicLoop->submit("session", {}, "payload"));
    logicLoop.reset();

    const auto observed = processed.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(processed.load() == observed);
}

}  // namespace

int main() {
    testProcessorRunsOnLogicThread();
    testSubmitWithResultReportsLifecycleState();
    testConcurrentSubmittersStillProcessOnSingleLogicThread();
    testDefaultOutputDispatchMetrics();
    testDefaultOutputDropsPayloadOverHardLimit();
    testDefaultOutputDropsOnlyLowPriorityAtSoftLimit();
    testLifecycleStartAndStopAreSafe();
    testTickBacklogAndLagContract();
    testStopPreventsProcessingAfterShutdown();
    testSubmitRejectedAfterStop();
    testLogicAdmissionRejectsHardBacklogAndEmitsMetric();
    testLogicAdmissionRejectsOldestLagAndDoesNotProcessRejectedCommand();
    testDestructorStopsRunningLoop();
    return 0;
}
