#include "mini/base/MetricsExporter.h"
#include "mini/net/InetAddress.h"

#include <cassert>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using namespace std::chrono_literals;

void testInMemoryCounterAndHistogramAggregation() {
    mini::base::InMemoryMetricsExporter exporter;

    exporter.incrementCounter("requests");
    exporter.incrementCounter("requests", 4);
    assert(exporter.counterValue("requests") == 5);
    assert(exporter.counterValue("missing") == 0);

    exporter.observeHistogram("latency_ms", 3.0);
    exporter.observeHistogram("latency_ms", 7.0);
    exporter.observeHistogram("latency_ms", 5.0);

    const auto histogram = exporter.histogram("latency_ms");
    assert(histogram.count == 3);
    assert(histogram.sum == 15.0);
    assert(histogram.min == 3.0);
    assert(histogram.max == 7.0);
    assert(histogram.average() == 5.0);

    const auto snapshot = exporter.snapshot();
    assert(snapshot.counters.size() == 1);
    assert(snapshot.histograms.size() == 1);

    exporter.reset();
    assert(exporter.counterValue("requests") == 0);
    assert(exporter.histogram("latency_ms").count == 0);
}

void testRecorderRejectsNullExporter() {
    try {
        mini::base::MetricsHookRecorder recorder(nullptr);
        assert(false && "recorder must reject null exporter");
    } catch (const std::invalid_argument&) {
    }
}

void testTaggedMetricNameValidationAndEscaping() {
    const mini::base::MetricLabels labels{
        {"service", "gateway"},
        {"shard_id", "alpha\"one\\two\nthree"},
    };

    const auto name = mini::base::metricNameWithLabels("mini.game.logic.tick_completed", labels);
    assert(name ==
           "mini.game.logic.tick_completed{service=\"gateway\",shard_id=\"alpha\\\"one\\\\two\\nthree\"}");

    try {
        (void)mini::base::metricNameWithLabels("", labels);
        assert(false && "empty metric name must be rejected");
    } catch (const std::invalid_argument&) {
    }

    try {
        (void)mini::base::metricNameWithLabels("already{labeled=\"yes\"}", labels);
        assert(false && "nested labels must be rejected");
    } catch (const std::invalid_argument&) {
    }

    try {
        (void)mini::base::metricNameWithLabels(
            "metric",
            mini::base::MetricLabels{{"1bad", "value"}});
        assert(false && "invalid label key must be rejected");
    } catch (const std::invalid_argument&) {
    }

    try {
        (void)mini::base::metricNameWithLabels(
            "metric",
            mini::base::MetricLabels{{"role", "a"}, {"role", "b"}});
        assert(false && "duplicate label keys must be rejected");
    } catch (const std::invalid_argument&) {
    }
}

void testTaggedExporterForwardsToSinkWithStaticLabels() {
    auto sink = std::make_shared<mini::base::InMemoryMetricsExporter>();
    auto tagged = std::make_shared<mini::base::TaggedMetricsExporter>(
        sink,
        mini::base::MetricLabels{{"service", "gateway"}, {"shard", "1"}});

    tagged->incrementCounter("mini.game.security.auth_rejected", 2);
    tagged->observeHistogram("mini.game.backpressure.queue_latency_ms", 3.5);

    assert(sink->counterValue(
               "mini.game.security.auth_rejected{service=\"gateway\",shard=\"1\"}") == 2);
    const auto histogram = sink->histogram(
        "mini.game.backpressure.queue_latency_ms{service=\"gateway\",shard=\"1\"}");
    assert(histogram.count == 1);
    assert(histogram.max == 3.5);

    try {
        mini::base::TaggedMetricsExporter bad(nullptr, {});
        assert(false && "tagged exporter must reject null sink");
    } catch (const std::invalid_argument&) {
    }
}

void testPrometheusTextRendererUsesStableNamesAndLabels() {
    auto exporter = std::make_shared<mini::base::InMemoryMetricsExporter>();
    auto tagged = std::make_shared<mini::base::TaggedMetricsExporter>(
        exporter,
        mini::base::MetricLabels{{"service", "gateway"}, {"shard", "blue"}});

    tagged->incrementCounter("mini.game.security.auth_rejected", 3);
    tagged->observeHistogram("mini.net.broadcast.payload_bytes", 128.0);
    tagged->observeHistogram("mini.net.broadcast.payload_bytes", 256.0);

    const auto text = mini::base::renderPrometheusText(exporter->snapshot());
    assert(text.find("# TYPE mini_game_security_auth_rejected counter\n") != std::string::npos);
    assert(text.find(
               "mini_game_security_auth_rejected{service=\"gateway\",shard=\"blue\"} 3\n") !=
           std::string::npos);
    assert(text.find("# TYPE mini_net_broadcast_payload_bytes summary\n") != std::string::npos);
    assert(text.find(
               "mini_net_broadcast_payload_bytes_count{service=\"gateway\",shard=\"blue\"} 2\n") !=
           std::string::npos);
    assert(text.find(
               "mini_net_broadcast_payload_bytes_sum{service=\"gateway\",shard=\"blue\"} 384\n") !=
           std::string::npos);
    assert(text.find("# TYPE mini_net_broadcast_payload_bytes_min gauge\n") != std::string::npos);
    assert(text.find(
               "mini_net_broadcast_payload_bytes_max{service=\"gateway\",shard=\"blue\"} 256\n") !=
           std::string::npos);
}

void testRecorderMapsHookSamplesToCountersAndHistograms() {
    auto exporter = std::make_shared<mini::base::InMemoryMetricsExporter>();
    mini::base::MetricsHookRecorder recorder(exporter);

    recorder.makeConnectionEventCallback()(nullptr, mini::net::ConnectionEvent::Connected);
    assert(exporter->counterValue("mini.net.connection.connected") == 1);

    recorder.makeBackpressureEventCallback()(nullptr, mini::net::BackpressureEvent::ReadPaused, 4096);
    assert(exporter->counterValue("mini.net.backpressure.read_paused") == 1);
    assert(exporter->histogram("mini.net.backpressure.buffered_bytes").max == 4096.0);

    mini::net::InetAddress address(12345, true);
    recorder.makeConnectorEventCallback()(address, mini::net::ConnectorEvent::ConnectTimeout);
    assert(exporter->counterValue("mini.net.connector.connect_timeout") == 1);

    recorder.makeTlsEventCallback()(nullptr, mini::net::TlsEvent::HandshakeFailed);
    assert(exporter->counterValue("mini.net.tls.handshake_failed") == 1);

    mini::net::BroadcastMetricSample broadcast;
    broadcast.event = mini::net::BroadcastMetricEvent::LoopFlushed;
    broadcast.requestedSessions = 4;
    broadcast.loopBatches = 2;
    broadcast.fanoutConnections = 4;
    broadcast.payloadBytes = 128;
    broadcast.priority = 2;
    broadcast.routeLatency = 1ms;
    broadcast.queueLatency = 2ms;
    broadcast.fanoutLatency = 3ms;
    recorder.makeBroadcastCallback()(broadcast);

    assert(exporter->counterValue("mini.net.broadcast.loop_flushed") == 1);
    assert(exporter->histogram("mini.net.broadcast.payload_bytes").max == 128.0);
    assert(exporter->histogram("mini.net.broadcast.priority").max == 2.0);
    assert(exporter->histogram("mini.net.broadcast.queue_latency_ms").max == 2.0);

    mini::net::EventLoopMetricSample loop;
    loop.event = mini::net::EventLoopMetricEvent::PendingFunctorsDrained;
    loop.pendingFunctors = 3;
    loop.pendingFunctorPeak = 5;
    loop.wakeupCount = 7;
    loop.oldestPendingLatency = 4ms;
    recorder.makeEventLoopCallback()(loop);

    assert(exporter->counterValue("mini.net.event_loop.pending_functors_drained") == 1);
    assert(exporter->histogram("mini.net.event_loop.pending_functor_peak").max == 5.0);
    assert(exporter->histogram("mini.net.event_loop.oldest_pending_latency_ms").max == 4.0);

    mini::net::UdpMetricSample udp;
    udp.event = mini::net::UdpMetricEvent::ReadBatch;
    udp.datagramsRead = 8;
    udp.bytesRead = 256;
    udp.maxDatagramsPerRead = 8;
    udp.budgetExhausted = true;
    udp.readDuration = 6ms;
    recorder.makeUdpCallback()(udp);

    assert(exporter->counterValue("mini.net.udp.read_batch") == 1);
    assert(exporter->counterValue("mini.net.udp.budget_exhausted") == 1);
    assert(exporter->histogram("mini.net.udp.bytes_read").max == 256.0);

    mini::game::logic::LogicLoopMetricSample logic;
    logic.event = mini::game::logic::LogicLoopMetricEvent::TickCompleted;
    logic.backlog = 2;
    logic.drainedCommands = 1;
    logic.tickDuration = 5ms;
    logic.tickJitter = 1ms;
    recorder.makeLogicLoopCallback()(logic);

    assert(exporter->counterValue("mini.game.logic.tick_completed") == 1);
    assert(exporter->histogram("mini.game.logic.tick_duration_ms").max == 5.0);

    mini::game::GamePipelineMetricSample pipeline;
    pipeline.event = mini::game::GamePipelineMetricEvent::InputBatchProcessed;
    pipeline.framesDecoded = 16;
    pipeline.bytesConsumed = 512;
    pipeline.bufferedBytes = 32;
    pipeline.continuationScheduled = true;
    pipeline.batchDuration = 7ms;
    recorder.makeGamePipelineCallback()(pipeline);

    assert(exporter->counterValue("mini.game.pipeline.input_batch_processed") == 1);
    assert(exporter->counterValue("mini.game.pipeline.continuation_scheduled") == 1);
    assert(exporter->histogram("mini.game.pipeline.batch_duration_ms").max == 7.0);

    mini::game::SessionMetricSample session;
    session.event = mini::game::SessionMetricEvent::AsyncEventsDrained;
    session.pendingEvents = 9;
    session.drainedEvents = 9;
    session.oldestEventLag = 8ms;
    recorder.makeSessionCallback()(session);

    assert(exporter->counterValue("mini.game.session.async_events_drained") == 1);
    assert(exporter->histogram("mini.game.session.drained_events").max == 9.0);

    mini::game::GameBackpressureMetricSample backpressure;
    backpressure.event = mini::game::GameBackpressureMetricEvent::BroadcastRejected;
    backpressure.currentValue = 12;
    backpressure.hardLimit = 10;
    backpressure.fanoutConnections = 12;
    backpressure.payloadBytes = 64;
    backpressure.priority = 3;
    backpressure.queueLatency = 9ms;
    recorder.makeGameBackpressureCallback()(backpressure);

    assert(exporter->counterValue("mini.game.backpressure.broadcast_rejected") == 1);
    assert(exporter->histogram("mini.game.backpressure.current_value").max == 12.0);
    assert(exporter->histogram("mini.game.backpressure.priority").max == 3.0);
    assert(exporter->histogram("mini.game.backpressure.queue_latency_ms").max == 9.0);

    mini::game::GameSecurityMetricSample security;
    security.event = mini::game::GameSecurityMetricEvent::AuthRejected;
    security.reason = mini::game::GameSecurityReason::AuthReplay;
    security.payloadBytes = 24;
    security.currentValue = 1;
    security.limit = 1;
    recorder.makeGameSecurityCallback()(security);

    assert(exporter->counterValue("mini.game.security.auth_rejected") == 1);
    assert(exporter->counterValue("mini.game.security.reason.auth_replay") == 1);
    assert(exporter->histogram("mini.game.security.payload_bytes").max == 24.0);
    assert(exporter->histogram("mini.game.security.limit").max == 1.0);
}

}  // namespace

int main() {
    testInMemoryCounterAndHistogramAggregation();
    testRecorderRejectsNullExporter();
    testTaggedMetricNameValidationAndEscaping();
    testTaggedExporterForwardsToSinkWithStaticLabels();
    testPrometheusTextRendererUsesStableNamesAndLabels();
    testRecorderMapsHookSamplesToCountersAndHistograms();
    return 0;
}
