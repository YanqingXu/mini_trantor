#include "mini/game/logic/LogicLoop.h"

#include <cassert>
#include <chrono>
#include <atomic>
#include <future>
#include <thread>

namespace {

void waitFutureReady(std::future<void>& future, std::chrono::milliseconds timeout) {
    assert(future.wait_for(timeout) == std::future_status::ready);
}

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
    assert(logicLoop.processedCount() == 1);
    assert(logicLoop.backlog() == 0);

    logicLoop.stop();
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

}  // namespace

int main() {
    testProcessorRunsOnLogicThread();
    testLifecycleStartAndStopAreSafe();
    testTickBacklogAndLagContract();
    testStopPreventsProcessingAfterShutdown();
    testSubmitRejectedAfterStop();
    return 0;
}
