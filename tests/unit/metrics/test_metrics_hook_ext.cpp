#include "mini/base/MetricsHook.h"
#include "mini/net/TcpServerOptions.h"

#include <cassert>
#include <chrono>
#include <stdexcept>
#include <string>

int main() {
    using namespace std::chrono_literals;

    mini::net::TcpServerOptions options;
    assert(!options.metrics.enableBroadcastMetrics);
    assert(!options.metrics.enableEventLoopQueueMetrics);

    options.metrics.enableBroadcastMetrics = true;
    options.metrics.enableEventLoopQueueMetrics = true;
    options.validate();

    options.numThreads = 0;
    try {
        options.validate();
        assert(false && "TcpServerOptions must reject numThreads < 1");
    } catch (const std::invalid_argument&) {
    }

    bool broadcastObserved = false;
    mini::net::BroadcastMetricCallback broadcastCallback =
        [&](const mini::net::BroadcastMetricSample& sample) {
            broadcastObserved = true;
            assert(sample.event == mini::net::BroadcastMetricEvent::LoopFlushed);
            assert(sample.payloadBytes == 64);
            assert(sample.fanoutConnections == 3);
            assert(sample.queueLatency >= mini::net::BroadcastMetricSample::Duration::zero());
        };
    mini::net::BroadcastMetricSample broadcastSample;
    broadcastSample.event = mini::net::BroadcastMetricEvent::LoopFlushed;
    broadcastSample.payloadBytes = 64;
    broadcastSample.fanoutConnections = 3;
    broadcastSample.queueLatency = 1ms;
    broadcastCallback(broadcastSample);
    assert(broadcastObserved);

    bool loopObserved = false;
    mini::net::EventLoopMetricCallback loopCallback =
        [&](const mini::net::EventLoopMetricSample& sample) {
            loopObserved = true;
            assert(sample.event == mini::net::EventLoopMetricEvent::PendingFunctorsDrained);
            assert(sample.pendingFunctorPeak >= sample.pendingFunctors);
            assert(sample.oldestPendingLatency >= mini::net::EventLoopMetricSample::Duration::zero());
        };
    mini::net::EventLoopMetricSample loopSample;
    loopSample.event = mini::net::EventLoopMetricEvent::PendingFunctorsDrained;
    loopSample.pendingFunctors = 2;
    loopSample.pendingFunctorPeak = 4;
    loopSample.oldestPendingLatency = 2ms;
    loopCallback(loopSample);
    assert(loopObserved);

    bool logicObserved = false;
    mini::game::logic::LogicLoopMetricCallback logicCallback =
        [&](const mini::game::logic::LogicLoopMetricSample& sample) {
            logicObserved = true;
            assert(sample.event == mini::game::logic::LogicLoopMetricEvent::TickCompleted);
            assert(sample.backlog == 1);
            assert(sample.tickJitter >= mini::game::logic::LogicLoopMetricSample::Duration::zero());
        };
    mini::game::logic::LogicLoopMetricSample logicSample;
    logicSample.event = mini::game::logic::LogicLoopMetricEvent::TickCompleted;
    logicSample.backlog = 1;
    logicSample.tickJitter = 3ms;
    logicCallback(logicSample);
    assert(logicObserved);

    bool sessionObserved = false;
    mini::game::SessionMetricCallback sessionCallback =
        [&](const mini::game::SessionMetricSample& sample) {
            sessionObserved = true;
            assert(sample.event == mini::game::SessionMetricEvent::ReconnectSucceeded);
            assert(sample.success);
            assert(sample.sessionToken == "s1");
            assert(sample.reconnectDuration >= mini::game::SessionMetricSample::Duration::zero());
        };
    mini::game::SessionMetricSample sessionSample;
    sessionSample.event = mini::game::SessionMetricEvent::ReconnectSucceeded;
    sessionSample.sessionToken = "s1";
    sessionSample.success = true;
    sessionSample.reconnectDuration = 5ms;
    sessionCallback(sessionSample);
    assert(sessionObserved);

    return 0;
}
