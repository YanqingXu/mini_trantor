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

    bool logicOutputObserved = false;
    mini::game::logic::LogicLoopMetricCallback logicOutputCallback =
        [&](const mini::game::logic::LogicLoopMetricSample& sample) {
            logicOutputObserved = true;
            assert(sample.event == mini::game::logic::LogicLoopMetricEvent::OutputDispatched);
            assert(sample.outputBatch == 2);
            assert(sample.queuedOutputs == 1);
            assert(sample.droppedOutputs == 1);
            assert(sample.outputBytes == 32);
        };
    mini::game::logic::LogicLoopMetricSample logicOutputSample;
    logicOutputSample.event = mini::game::logic::LogicLoopMetricEvent::OutputDispatched;
    logicOutputSample.outputBatch = 2;
    logicOutputSample.queuedOutputs = 1;
    logicOutputSample.droppedOutputs = 1;
    logicOutputSample.outputBytes = 32;
    logicOutputCallback(logicOutputSample);
    assert(logicOutputObserved);

    bool pipelineObserved = false;
    mini::game::GamePipelineMetricCallback pipelineCallback =
        [&](const mini::game::GamePipelineMetricSample& sample) {
            pipelineObserved = true;
            assert(sample.event == mini::game::GamePipelineMetricEvent::InputBatchProcessed);
            assert(sample.framesDecoded == 3);
            assert(sample.bytesConsumed == 128);
            assert(sample.bufferedBytes == 16);
            assert(sample.continuationScheduled);
            assert(sample.batchDuration >= mini::game::GamePipelineMetricSample::Duration::zero());
        };
    mini::game::GamePipelineMetricSample pipelineSample;
    pipelineSample.event = mini::game::GamePipelineMetricEvent::InputBatchProcessed;
    pipelineSample.framesDecoded = 3;
    pipelineSample.bytesConsumed = 128;
    pipelineSample.bufferedBytes = 16;
    pipelineSample.continuationScheduled = true;
    pipelineSample.batchDuration = 2ms;
    pipelineCallback(pipelineSample);
    assert(pipelineObserved);

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

    bool asyncSessionObserved = false;
    mini::game::SessionMetricCallback asyncSessionCallback =
        [&](const mini::game::SessionMetricSample& sample) {
            asyncSessionObserved = true;
            assert(sample.event == mini::game::SessionMetricEvent::AsyncEventsDrained);
            assert(sample.pendingEvents == 8);
            assert(sample.drainedEvents == 8);
            assert(sample.oldestEventLag >= mini::game::SessionMetricSample::Duration::zero());
        };
    mini::game::SessionMetricSample asyncSessionSample;
    asyncSessionSample.event = mini::game::SessionMetricEvent::AsyncEventsDrained;
    asyncSessionSample.pendingEvents = 8;
    asyncSessionSample.drainedEvents = 8;
    asyncSessionSample.oldestEventLag = 4ms;
    asyncSessionCallback(asyncSessionSample);
    assert(asyncSessionObserved);

    bool udpObserved = false;
    mini::net::UdpMetricCallback udpCallback =
        [&](const mini::net::UdpMetricSample& sample) {
            udpObserved = true;
            assert(sample.event == mini::net::UdpMetricEvent::ReadBatch);
            assert(sample.socketName == "udp-metrics");
            assert(sample.datagramsRead == 4);
            assert(sample.bytesRead == 64);
            assert(sample.maxDatagramsPerRead == 4);
            assert(sample.budgetExhausted);
            assert(sample.readDuration >= mini::net::UdpMetricSample::Duration::zero());
        };
    mini::net::UdpMetricSample udpSample;
    udpSample.event = mini::net::UdpMetricEvent::ReadBatch;
    udpSample.socketName = "udp-metrics";
    udpSample.datagramsRead = 4;
    udpSample.bytesRead = 64;
    udpSample.maxDatagramsPerRead = 4;
    udpSample.budgetExhausted = true;
    udpSample.readDuration = 1ms;
    udpCallback(udpSample);
    assert(udpObserved);

    bool gameBackpressureObserved = false;
    mini::game::GameBackpressureMetricCallback gameBackpressureCallback =
        [&](const mini::game::GameBackpressureMetricSample& sample) {
            gameBackpressureObserved = true;
            assert(sample.event == mini::game::GameBackpressureMetricEvent::LogicRejected);
            assert(sample.layer == mini::game::GameBackpressureLayer::LogicAdmission);
            assert(sample.action == mini::game::GameBackpressureAction::Reject);
            assert(sample.reason == mini::game::GameBackpressureReason::LogicBacklogHardLimit);
            assert(sample.sessionToken == "s2");
            assert(sample.backlog == 129);
            assert(sample.hardLimit == 128);
        };
    mini::game::GameBackpressureMetricSample gameBackpressureSample;
    gameBackpressureSample.event = mini::game::GameBackpressureMetricEvent::LogicRejected;
    gameBackpressureSample.layer = mini::game::GameBackpressureLayer::LogicAdmission;
    gameBackpressureSample.action = mini::game::GameBackpressureAction::Reject;
    gameBackpressureSample.reason = mini::game::GameBackpressureReason::LogicBacklogHardLimit;
    gameBackpressureSample.sessionToken = "s2";
    gameBackpressureSample.backlog = 129;
    gameBackpressureSample.hardLimit = 128;
    gameBackpressureCallback(gameBackpressureSample);
    assert(gameBackpressureObserved);

    bool gameSecurityObserved = false;
    mini::game::GameSecurityMetricCallback gameSecurityCallback =
        [&](const mini::game::GameSecurityMetricSample& sample) {
            gameSecurityObserved = true;
            assert(sample.event == mini::game::GameSecurityMetricEvent::RateLimited);
            assert(sample.reason == mini::game::GameSecurityReason::SessionRateLimit);
            assert(sample.sessionToken == "s3");
            assert(sample.msgId == 2);
            assert(sample.currentValue == 3);
            assert(sample.limit == 2);
        };
    mini::game::GameSecurityMetricSample gameSecuritySample;
    gameSecuritySample.event = mini::game::GameSecurityMetricEvent::RateLimited;
    gameSecuritySample.reason = mini::game::GameSecurityReason::SessionRateLimit;
    gameSecuritySample.sessionToken = "s3";
    gameSecuritySample.msgId = 2;
    gameSecuritySample.currentValue = 3;
    gameSecuritySample.limit = 2;
    gameSecurityCallback(gameSecuritySample);
    assert(gameSecurityObserved);

    return 0;
}
