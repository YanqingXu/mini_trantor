#include "mini/base/MetricsExporter.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"

#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void testRecorderCallbacksOwnExporterNotRecorder() {
    auto exporter = std::make_shared<mini::base::InMemoryMetricsExporter>();
    mini::net::BroadcastMetricCallback callback;
    {
        mini::base::MetricsHookRecorder recorder(exporter);
        callback = recorder.makeBroadcastCallback();
    }

    mini::net::BroadcastMetricSample sample;
    sample.event = mini::net::BroadcastMetricEvent::Routed;
    sample.fanoutConnections = 2;
    sample.payloadBytes = 32;
    callback(sample);

    assert(exporter->counterValue("mini.net.broadcast.routed") == 1);
    assert(exporter->histogram("mini.net.broadcast.fanout_connections").max == 2.0);
}

void testInMemoryExporterIsSafeForConcurrentHooks() {
    mini::base::InMemoryMetricsExporter exporter;

    constexpr int threadCount = 4;
    constexpr int iterations = 1000;
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back([&exporter] {
            for (int j = 0; j < iterations; ++j) {
                exporter.incrementCounter("concurrent.counter");
                exporter.observeHistogram("concurrent.histogram", static_cast<double>(j));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    assert(exporter.counterValue("concurrent.counter") ==
           static_cast<std::uint64_t>(threadCount * iterations));
    const auto histogram = exporter.histogram("concurrent.histogram");
    assert(histogram.count == static_cast<std::uint64_t>(threadCount * iterations));
    assert(histogram.min == 0.0);
    assert(histogram.max == static_cast<double>(iterations - 1));
}

void testTaggedExporterIsSafeForConcurrentHooks() {
    auto sink = std::make_shared<mini::base::InMemoryMetricsExporter>();
    mini::base::TaggedMetricsExporter exporter(
        sink,
        mini::base::MetricLabels{{"service", "game-gateway"}, {"shard", "7"}});

    constexpr int threadCount = 4;
    constexpr int iterations = 1000;
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back([&exporter] {
            for (int j = 0; j < iterations; ++j) {
                exporter.incrementCounter("mini.game.logic.tick_completed");
                exporter.observeHistogram("mini.game.logic.tick_duration_ms",
                                          static_cast<double>(j));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    const auto counterName =
        "mini.game.logic.tick_completed{service=\"game-gateway\",shard=\"7\"}";
    const auto histogramName =
        "mini.game.logic.tick_duration_ms{service=\"game-gateway\",shard=\"7\"}";
    assert(sink->counterValue(counterName) ==
           static_cast<std::uint64_t>(threadCount * iterations));
    const auto histogram = sink->histogram(histogramName);
    assert(histogram.count == static_cast<std::uint64_t>(threadCount * iterations));
    assert(histogram.max == static_cast<double>(iterations - 1));
}

void testEventLoopMetricsCanBeRecordedFromOwnerLoopHook() {
    const auto callerThread = std::this_thread::get_id();
    auto exporter = std::make_shared<mini::base::InMemoryMetricsExporter>();
    mini::base::MetricsHookRecorder recorder(exporter);

    std::promise<std::thread::id> callbackThreadPromise;
    auto callbackThreadFuture = callbackThreadPromise.get_future();
    std::once_flag callbackOnce;

    mini::net::EventLoopThread loopThread([&](mini::net::EventLoop* loop) {
        auto callback = recorder.makeEventLoopCallback();
        loop->setEventLoopMetricCallback(
            [callback = std::move(callback), &callbackThreadPromise, &callbackOnce](
                const mini::net::EventLoopMetricSample& sample) mutable {
                callback(sample);
                if (sample.event == mini::net::EventLoopMetricEvent::PendingFunctorsDrained) {
                    std::call_once(callbackOnce, [&] {
                        callbackThreadPromise.set_value(std::this_thread::get_id());
                    });
                }
            });
    });

    auto* loop = loopThread.startLoop();
    std::promise<void> ranPromise;
    auto ranFuture = ranPromise.get_future();
    loop->queueInLoop([&ranPromise] { ranPromise.set_value(); });

    assert(ranFuture.wait_for(1s) == std::future_status::ready);
    assert(callbackThreadFuture.wait_for(1s) == std::future_status::ready);
    assert(callbackThreadFuture.get() != callerThread);
    assert(exporter->counterValue("mini.net.event_loop.pending_functors_drained") >= 1);
    assert(exporter->histogram("mini.net.event_loop.pending_functors").count >= 1);

    loop->quit();
}

void testTaggedRecorderPreservesOwnerLoopHookThread() {
    const auto callerThread = std::this_thread::get_id();
    auto sink = std::make_shared<mini::base::InMemoryMetricsExporter>();
    auto tagged = std::make_shared<mini::base::TaggedMetricsExporter>(
        sink,
        mini::base::MetricLabels{{"service", "game-gateway"}, {"role", "base-loop"}});
    mini::base::MetricsHookRecorder recorder(tagged);

    std::promise<std::thread::id> callbackThreadPromise;
    auto callbackThreadFuture = callbackThreadPromise.get_future();
    std::once_flag callbackOnce;

    mini::net::EventLoopThread loopThread([&](mini::net::EventLoop* loop) {
        auto callback = recorder.makeEventLoopCallback();
        loop->setEventLoopMetricCallback(
            [callback = std::move(callback), &callbackThreadPromise, &callbackOnce](
                const mini::net::EventLoopMetricSample& sample) mutable {
                callback(sample);
                if (sample.event == mini::net::EventLoopMetricEvent::PendingFunctorsDrained) {
                    std::call_once(callbackOnce, [&] {
                        callbackThreadPromise.set_value(std::this_thread::get_id());
                    });
                }
            });
    });

    auto* loop = loopThread.startLoop();
    std::promise<void> ranPromise;
    auto ranFuture = ranPromise.get_future();
    loop->queueInLoop([&ranPromise] { ranPromise.set_value(); });

    assert(ranFuture.wait_for(1s) == std::future_status::ready);
    assert(callbackThreadFuture.wait_for(1s) == std::future_status::ready);
    assert(callbackThreadFuture.get() != callerThread);
    assert(sink->counterValue(
               "mini.net.event_loop.pending_functors_drained{service=\"game-gateway\",role=\"base-loop\"}") >=
           1);
    assert(sink->histogram(
               "mini.net.event_loop.pending_functors{service=\"game-gateway\",role=\"base-loop\"}").count >=
           1);

    loop->quit();
}

}  // namespace

int main() {
    testRecorderCallbacksOwnExporterNotRecorder();
    testInMemoryExporterIsSafeForConcurrentHooks();
    testTaggedExporterIsSafeForConcurrentHooks();
    testEventLoopMetricsCanBeRecordedFromOwnerLoopHook();
    testTaggedRecorderPreservesOwnerLoopHookThread();
    return 0;
}
