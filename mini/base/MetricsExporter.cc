#include "mini/base/MetricsExporter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace mini::base {

namespace {

template <typename DurationT>
double toMilliseconds(DurationT duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

void observeDurationMs(MetricsExporter& exporter, std::string_view name,
                       std::chrono::steady_clock::duration duration) {
    exporter.observeHistogram(name, toMilliseconds(duration));
}

void observeDurationMs(MetricsExporter& exporter, std::string_view name,
                       std::chrono::milliseconds duration) {
    exporter.observeHistogram(name, static_cast<double>(duration.count()));
}

std::string_view eventName(mini::net::ConnectionEvent event) {
    switch (event) {
    case mini::net::ConnectionEvent::Connected:
        return "connected";
    case mini::net::ConnectionEvent::Disconnected:
        return "disconnected";
    case mini::net::ConnectionEvent::IdleTimeout:
        return "idle_timeout";
    case mini::net::ConnectionEvent::ForceClosed:
        return "force_closed";
    }
    return "unknown";
}

std::string_view eventName(mini::net::BackpressureEvent event) {
    switch (event) {
    case mini::net::BackpressureEvent::ReadPaused:
        return "read_paused";
    case mini::net::BackpressureEvent::ReadResumed:
        return "read_resumed";
    }
    return "unknown";
}

std::string_view eventName(mini::net::ConnectorEvent event) {
    switch (event) {
    case mini::net::ConnectorEvent::ConnectAttempt:
        return "connect_attempt";
    case mini::net::ConnectorEvent::ConnectSuccess:
        return "connect_success";
    case mini::net::ConnectorEvent::ConnectFailed:
        return "connect_failed";
    case mini::net::ConnectorEvent::RetryScheduled:
        return "retry_scheduled";
    case mini::net::ConnectorEvent::SelfConnectDetected:
        return "self_connect_detected";
    case mini::net::ConnectorEvent::ConnectTimeout:
        return "connect_timeout";
    }
    return "unknown";
}

std::string_view eventName(mini::net::TlsEvent event) {
    switch (event) {
    case mini::net::TlsEvent::HandshakeStarted:
        return "handshake_started";
    case mini::net::TlsEvent::HandshakeCompleted:
        return "handshake_completed";
    case mini::net::TlsEvent::HandshakeFailed:
        return "handshake_failed";
    }
    return "unknown";
}

std::string_view eventName(mini::net::BroadcastMetricEvent event) {
    switch (event) {
    case mini::net::BroadcastMetricEvent::Routed:
        return "routed";
    case mini::net::BroadcastMetricEvent::LoopFlushed:
        return "loop_flushed";
    }
    return "unknown";
}

std::string_view eventName(mini::net::EventLoopMetricEvent event) {
    switch (event) {
    case mini::net::EventLoopMetricEvent::PendingFunctorsDrained:
        return "pending_functors_drained";
    case mini::net::EventLoopMetricEvent::WakeupHandled:
        return "wakeup_handled";
    }
    return "unknown";
}

std::string_view eventName(mini::net::UdpMetricEvent event) {
    switch (event) {
    case mini::net::UdpMetricEvent::ReadBatch:
        return "read_batch";
    }
    return "unknown";
}

std::string_view eventName(mini::game::logic::LogicLoopMetricEvent event) {
    switch (event) {
    case mini::game::logic::LogicLoopMetricEvent::CommandEnqueued:
        return "command_enqueued";
    case mini::game::logic::LogicLoopMetricEvent::TickCompleted:
        return "tick_completed";
    case mini::game::logic::LogicLoopMetricEvent::OutputDispatched:
        return "output_dispatched";
    case mini::game::logic::LogicLoopMetricEvent::OutputSent:
        return "output_sent";
    }
    return "unknown";
}

std::string_view eventName(mini::game::GamePipelineMetricEvent event) {
    switch (event) {
    case mini::game::GamePipelineMetricEvent::InputBatchProcessed:
        return "input_batch_processed";
    case mini::game::GamePipelineMetricEvent::LogicSubmitResult:
        return "logic_submit_result";
    }
    return "unknown";
}

std::string_view eventName(mini::game::SessionMetricEvent event) {
    switch (event) {
    case mini::game::SessionMetricEvent::ReconnectWindowStarted:
        return "reconnect_window_started";
    case mini::game::SessionMetricEvent::ReconnectSucceeded:
        return "reconnect_succeeded";
    case mini::game::SessionMetricEvent::ReconnectExpired:
        return "reconnect_expired";
    case mini::game::SessionMetricEvent::AsyncEventsDrained:
        return "async_events_drained";
    }
    return "unknown";
}

std::string_view eventName(mini::game::GameBackpressureMetricEvent event) {
    switch (event) {
    case mini::game::GameBackpressureMetricEvent::InputDeferred:
        return "input_deferred";
    case mini::game::GameBackpressureMetricEvent::InputRejected:
        return "input_rejected";
    case mini::game::GameBackpressureMetricEvent::LogicAccepted:
        return "logic_accepted";
    case mini::game::GameBackpressureMetricEvent::LogicRejected:
        return "logic_rejected";
    case mini::game::GameBackpressureMetricEvent::OutputQueued:
        return "output_queued";
    case mini::game::GameBackpressureMetricEvent::OutputDropped:
        return "output_dropped";
    case mini::game::GameBackpressureMetricEvent::BroadcastAccepted:
        return "broadcast_accepted";
    case mini::game::GameBackpressureMetricEvent::BroadcastDeferred:
        return "broadcast_deferred";
    case mini::game::GameBackpressureMetricEvent::BroadcastRejected:
        return "broadcast_rejected";
    }
    return "unknown";
}

std::string_view eventName(mini::game::GameSecurityMetricEvent event) {
    switch (event) {
    case mini::game::GameSecurityMetricEvent::AuthAccepted:
        return "auth_accepted";
    case mini::game::GameSecurityMetricEvent::AuthRejected:
        return "auth_rejected";
    case mini::game::GameSecurityMetricEvent::RateLimited:
        return "rate_limited";
    case mini::game::GameSecurityMetricEvent::AbnormalClose:
        return "abnormal_close";
    }
    return "unknown";
}

std::string_view reasonName(mini::game::GameSecurityReason reason) {
    switch (reason) {
    case mini::game::GameSecurityReason::None:
        return "none";
    case mini::game::GameSecurityReason::EmptyAuthToken:
        return "empty_auth_token";
    case mini::game::GameSecurityReason::EmptyAuthNonce:
        return "empty_auth_nonce";
    case mini::game::GameSecurityReason::AuthTokenTooLarge:
        return "auth_token_too_large";
    case mini::game::GameSecurityReason::AuthTokenValidatorRejected:
        return "auth_token_validator_rejected";
    case mini::game::GameSecurityReason::AuthReplay:
        return "auth_replay";
    case mini::game::GameSecurityReason::SessionEnsureFailed:
        return "session_ensure_failed";
    case mini::game::GameSecurityReason::UnauthenticatedFrame:
        return "unauthenticated_frame";
    case mini::game::GameSecurityReason::SessionRateLimit:
        return "session_rate_limit";
    case mini::game::GameSecurityReason::InvalidFrame:
        return "invalid_frame";
    case mini::game::GameSecurityReason::PipelineStateMissing:
        return "pipeline_state_missing";
    }
    return "unknown";
}

std::string counterName(std::string_view prefix, std::string_view event) {
    std::string name(prefix);
    name.push_back('.');
    name.append(event);
    return name;
}

bool isLabelKeyStart(char ch) noexcept {
    const auto c = static_cast<unsigned char>(ch);
    return std::isalpha(c) || ch == '_';
}

bool isLabelKeyRest(char ch) noexcept {
    const auto c = static_cast<unsigned char>(ch);
    return std::isalnum(c) || ch == '_';
}

void validateMetricBaseName(std::string_view name) {
    if (name.empty()) {
        throw std::invalid_argument("metric name must not be empty");
    }
    if (name.find('{') != std::string_view::npos ||
        name.find('}') != std::string_view::npos) {
        throw std::invalid_argument("metric name must not contain label braces");
    }
}

void validateLabels(const MetricLabels& labels) {
    std::unordered_set<std::string> seen;
    for (const auto& label : labels) {
        if (label.key.empty() || !isLabelKeyStart(label.key.front())) {
            throw std::invalid_argument("metric label key must start with [A-Za-z_]");
        }
        for (const auto ch : label.key) {
            if (!isLabelKeyRest(ch)) {
                throw std::invalid_argument("metric label key must contain only [A-Za-z0-9_]");
            }
        }
        if (!seen.insert(label.key).second) {
            throw std::invalid_argument("metric labels must not contain duplicate keys");
        }
    }
}

std::string escapeLabelValue(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
        case '\\':
            escaped.append("\\\\");
            break;
        case '"':
            escaped.append("\\\"");
            break;
        case '\n':
            escaped.append("\\n");
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::string labelSuffixFromLabels(const MetricLabels& labels) {
    validateLabels(labels);
    if (labels.empty()) {
        return {};
    }

    std::string suffix;
    suffix.push_back('{');
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) {
            suffix.push_back(',');
        }
        suffix.append(labels[i].key);
        suffix.append("=\"");
        suffix.append(escapeLabelValue(labels[i].value));
        suffix.push_back('"');
    }
    suffix.push_back('}');
    return suffix;
}

std::string appendLabelSuffix(std::string_view name, std::string_view suffix) {
    validateMetricBaseName(name);
    std::string tagged(name);
    tagged.append(suffix);
    return tagged;
}

std::string sanitizePrometheusMetricName(std::string_view name) {
    std::string sanitized;
    sanitized.reserve(name.size() + 1);
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '_' || ch == ':') {
            sanitized.push_back(ch);
        } else {
            sanitized.push_back('_');
        }
    }
    if (sanitized.empty()) {
        sanitized = "_";
    }
    const auto first = static_cast<unsigned char>(sanitized.front());
    if (std::isdigit(first)) {
        sanitized.insert(sanitized.begin(), '_');
    }
    return sanitized;
}

struct ParsedMetricName {
    std::string baseName;
    std::string labelSuffix;
};

ParsedMetricName parseMetricName(std::string_view name) {
    const auto open = name.find('{');
    if (open == std::string_view::npos || name.empty() || name.back() != '}') {
        return {std::string(name), {}};
    }
    return {std::string(name.substr(0, open)), std::string(name.substr(open))};
}

std::string formatDouble(double value) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value > 0 ? "+Inf" : "-Inf";
    }

    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return stream.str();
}

void emitTypeOnce(std::ostringstream& out,
                  std::unordered_set<std::string>& emitted,
                  std::string_view metricName,
                  std::string_view metricType) {
    std::string key(metricName);
    key.push_back(':');
    key.append(metricType);
    if (!emitted.insert(std::move(key)).second) {
        return;
    }
    out << "# TYPE " << metricName << ' ' << metricType << '\n';
}

void emitSample(std::ostringstream& out,
                std::string_view metricName,
                std::string_view labelSuffix,
                std::string_view value) {
    out << metricName << labelSuffix << ' ' << value << '\n';
}

}  // namespace

void InMemoryMetricsExporter::incrementCounter(std::string_view name, std::uint64_t delta) {
    std::lock_guard lock(mutex_);
    counters_[std::string(name)] += delta;
}

void InMemoryMetricsExporter::observeHistogram(std::string_view name, double value) {
    std::lock_guard lock(mutex_);
    auto key = std::string(name);
    auto& snapshot = histograms_[key];
    if (snapshot.name.empty()) {
        snapshot.name = std::move(key);
    }
    if (snapshot.count == 0) {
        snapshot.min = value;
        snapshot.max = value;
    } else {
        if (value < snapshot.min) {
            snapshot.min = value;
        }
        if (value > snapshot.max) {
            snapshot.max = value;
        }
    }
    ++snapshot.count;
    snapshot.sum += value;
}

std::uint64_t InMemoryMetricsExporter::counterValue(std::string_view name) const {
    std::lock_guard lock(mutex_);
    const auto it = counters_.find(std::string(name));
    return it == counters_.end() ? 0 : it->second;
}

HistogramSnapshot InMemoryMetricsExporter::histogram(std::string_view name) const {
    std::lock_guard lock(mutex_);
    const auto it = histograms_.find(std::string(name));
    if (it == histograms_.end()) {
        HistogramSnapshot empty;
        empty.name = std::string(name);
        return empty;
    }
    return it->second;
}

MetricsSnapshot InMemoryMetricsExporter::snapshot() const {
    std::lock_guard lock(mutex_);
    MetricsSnapshot snapshot;
    snapshot.counters.reserve(counters_.size());
    snapshot.histograms.reserve(histograms_.size());

    for (const auto& [name, value] : counters_) {
        snapshot.counters.push_back(CounterSnapshot{name, value});
    }
    for (const auto& [name, histogram] : histograms_) {
        snapshot.histograms.push_back(histogram);
    }
    return snapshot;
}

void InMemoryMetricsExporter::reset() {
    std::lock_guard lock(mutex_);
    counters_.clear();
    histograms_.clear();
}

TaggedMetricsExporter::TaggedMetricsExporter(std::shared_ptr<MetricsExporter> sink,
                                             MetricLabels labels)
    : sink_(std::move(sink)),
      labels_(std::move(labels)),
      labelSuffix_(labelSuffixFromLabels(labels_)) {
    if (!sink_) {
        throw std::invalid_argument("TaggedMetricsExporter requires a non-null sink exporter");
    }
}

void TaggedMetricsExporter::incrementCounter(std::string_view name, std::uint64_t delta) {
    sink_->incrementCounter(appendLabelSuffix(name, labelSuffix_), delta);
}

void TaggedMetricsExporter::observeHistogram(std::string_view name, double value) {
    sink_->observeHistogram(appendLabelSuffix(name, labelSuffix_), value);
}

std::string metricNameWithLabels(std::string_view name, const MetricLabels& labels) {
    return appendLabelSuffix(name, labelSuffixFromLabels(labels));
}

std::string renderPrometheusText(const MetricsSnapshot& snapshot) {
    auto counters = snapshot.counters;
    auto histograms = snapshot.histograms;
    std::sort(counters.begin(), counters.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.name < rhs.name;
    });
    std::sort(histograms.begin(), histograms.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.name < rhs.name;
    });

    std::ostringstream out;
    std::unordered_set<std::string> emittedTypes;
    for (const auto& counter : counters) {
        const auto parsed = parseMetricName(counter.name);
        const auto metricName = sanitizePrometheusMetricName(parsed.baseName);
        emitTypeOnce(out, emittedTypes, metricName, "counter");
        emitSample(out, metricName, parsed.labelSuffix, std::to_string(counter.value));
    }

    for (const auto& histogram : histograms) {
        const auto parsed = parseMetricName(histogram.name);
        const auto metricName = sanitizePrometheusMetricName(parsed.baseName);
        emitTypeOnce(out, emittedTypes, metricName, "summary");
        emitSample(out,
                   metricName + "_count",
                   parsed.labelSuffix,
                   std::to_string(histogram.count));
        emitSample(out,
                   metricName + "_sum",
                   parsed.labelSuffix,
                   formatDouble(histogram.sum));

        const auto minName = metricName + "_min";
        const auto maxName = metricName + "_max";
        emitTypeOnce(out, emittedTypes, minName, "gauge");
        emitSample(out, minName, parsed.labelSuffix, formatDouble(histogram.min));
        emitTypeOnce(out, emittedTypes, maxName, "gauge");
        emitSample(out, maxName, parsed.labelSuffix, formatDouble(histogram.max));
    }

    return out.str();
}

MetricsHookRecorder::MetricsHookRecorder(std::shared_ptr<MetricsExporter> exporter)
    : exporter_(std::move(exporter)) {
    if (!exporter_) {
        throw std::invalid_argument("MetricsHookRecorder requires a non-null exporter");
    }
}

mini::net::ConnectionEventCallback MetricsHookRecorder::makeConnectionEventCallback() const {
    auto exporter = exporter_;
    return [exporter = std::move(exporter)](
               const std::shared_ptr<mini::net::TcpConnection>&,
               mini::net::ConnectionEvent event) {
        record(*exporter, event);
    };
}

mini::net::BackpressureEventCallback MetricsHookRecorder::makeBackpressureEventCallback() const {
    auto exporter = exporter_;
    return [exporter = std::move(exporter)](
               const std::shared_ptr<mini::net::TcpConnection>&,
               mini::net::BackpressureEvent event,
               std::size_t bufferedBytes) {
        record(*exporter, event, bufferedBytes);
    };
}

mini::net::ConnectorEventCallback MetricsHookRecorder::makeConnectorEventCallback() const {
    auto exporter = exporter_;
    return [exporter = std::move(exporter)](
               const mini::net::InetAddress&,
               mini::net::ConnectorEvent event) {
        record(*exporter, event);
    };
}

mini::net::TlsEventCallback MetricsHookRecorder::makeTlsEventCallback() const {
    auto exporter = exporter_;
    return [exporter = std::move(exporter)](
               const std::shared_ptr<mini::net::TcpConnection>&,
               mini::net::TlsEvent event) {
        record(*exporter, event);
    };
}

mini::net::BroadcastMetricCallback MetricsHookRecorder::makeBroadcastCallback() const {
    auto exporter = exporter_;
    return [exporter = std::move(exporter)](const mini::net::BroadcastMetricSample& sample) {
        record(*exporter, sample);
    };
}

mini::net::EventLoopMetricCallback MetricsHookRecorder::makeEventLoopCallback() const {
    auto exporter = exporter_;
    return [exporter = std::move(exporter)](const mini::net::EventLoopMetricSample& sample) {
        record(*exporter, sample);
    };
}

mini::net::UdpMetricCallback MetricsHookRecorder::makeUdpCallback() const {
    auto exporter = exporter_;
    return [exporter = std::move(exporter)](const mini::net::UdpMetricSample& sample) {
        record(*exporter, sample);
    };
}

mini::game::logic::LogicLoopMetricCallback MetricsHookRecorder::makeLogicLoopCallback() const {
    auto exporter = exporter_;
    return [exporter = std::move(exporter)](const mini::game::logic::LogicLoopMetricSample& sample) {
        record(*exporter, sample);
    };
}

mini::game::GamePipelineMetricCallback MetricsHookRecorder::makeGamePipelineCallback() const {
    auto exporter = exporter_;
    return [exporter = std::move(exporter)](const mini::game::GamePipelineMetricSample& sample) {
        record(*exporter, sample);
    };
}

mini::game::SessionMetricCallback MetricsHookRecorder::makeSessionCallback() const {
    auto exporter = exporter_;
    return [exporter = std::move(exporter)](const mini::game::SessionMetricSample& sample) {
        record(*exporter, sample);
    };
}

mini::game::GameBackpressureMetricCallback MetricsHookRecorder::makeGameBackpressureCallback() const {
    auto exporter = exporter_;
    return [exporter = std::move(exporter)](const mini::game::GameBackpressureMetricSample& sample) {
        record(*exporter, sample);
    };
}

mini::game::GameSecurityMetricCallback MetricsHookRecorder::makeGameSecurityCallback() const {
    auto exporter = exporter_;
    return [exporter = std::move(exporter)](const mini::game::GameSecurityMetricSample& sample) {
        record(*exporter, sample);
    };
}

void MetricsHookRecorder::record(MetricsExporter& exporter, mini::net::ConnectionEvent event) {
    exporter.incrementCounter(counterName("mini.net.connection", eventName(event)));
}

void MetricsHookRecorder::record(MetricsExporter& exporter,
                                 mini::net::BackpressureEvent event,
                                 std::size_t bufferedBytes) {
    exporter.incrementCounter(counterName("mini.net.backpressure", eventName(event)));
    exporter.observeHistogram("mini.net.backpressure.buffered_bytes", bufferedBytes);
}

void MetricsHookRecorder::record(MetricsExporter& exporter, mini::net::ConnectorEvent event) {
    exporter.incrementCounter(counterName("mini.net.connector", eventName(event)));
}

void MetricsHookRecorder::record(MetricsExporter& exporter, mini::net::TlsEvent event) {
    exporter.incrementCounter(counterName("mini.net.tls", eventName(event)));
}

void MetricsHookRecorder::record(MetricsExporter& exporter,
                                 const mini::net::BroadcastMetricSample& sample) {
    exporter.incrementCounter(counterName("mini.net.broadcast", eventName(sample.event)));
    exporter.observeHistogram("mini.net.broadcast.requested_sessions", sample.requestedSessions);
    exporter.observeHistogram("mini.net.broadcast.loop_batches", sample.loopBatches);
    exporter.observeHistogram("mini.net.broadcast.fanout_connections", sample.fanoutConnections);
    exporter.observeHistogram("mini.net.broadcast.payload_bytes", sample.payloadBytes);
    exporter.observeHistogram("mini.net.broadcast.priority", sample.priority);
    observeDurationMs(exporter, "mini.net.broadcast.route_latency_ms", sample.routeLatency);
    observeDurationMs(exporter, "mini.net.broadcast.queue_latency_ms", sample.queueLatency);
    observeDurationMs(exporter, "mini.net.broadcast.fanout_latency_ms", sample.fanoutLatency);
}

void MetricsHookRecorder::record(MetricsExporter& exporter,
                                 const mini::net::EventLoopMetricSample& sample) {
    exporter.incrementCounter(counterName("mini.net.event_loop", eventName(sample.event)));
    exporter.observeHistogram("mini.net.event_loop.pending_functors", sample.pendingFunctors);
    exporter.observeHistogram("mini.net.event_loop.pending_functor_peak", sample.pendingFunctorPeak);
    exporter.observeHistogram("mini.net.event_loop.wakeup_count", static_cast<double>(sample.wakeupCount));
    observeDurationMs(exporter, "mini.net.event_loop.oldest_pending_latency_ms", sample.oldestPendingLatency);
}

void MetricsHookRecorder::record(MetricsExporter& exporter,
                                 const mini::net::UdpMetricSample& sample) {
    exporter.incrementCounter(counterName("mini.net.udp", eventName(sample.event)));
    exporter.observeHistogram("mini.net.udp.datagrams_read", sample.datagramsRead);
    exporter.observeHistogram("mini.net.udp.bytes_read", sample.bytesRead);
    exporter.observeHistogram("mini.net.udp.max_datagrams_per_read", sample.maxDatagramsPerRead);
    if (sample.budgetExhausted) {
        exporter.incrementCounter("mini.net.udp.budget_exhausted");
    }
    observeDurationMs(exporter, "mini.net.udp.read_duration_ms", sample.readDuration);
}

void MetricsHookRecorder::record(MetricsExporter& exporter,
                                 const mini::game::logic::LogicLoopMetricSample& sample) {
    exporter.incrementCounter(counterName("mini.game.logic", eventName(sample.event)));
    exporter.observeHistogram("mini.game.logic.backlog", sample.backlog);
    exporter.observeHistogram("mini.game.logic.drained_commands", sample.drainedCommands);
    exporter.observeHistogram("mini.game.logic.output_batch", sample.outputBatch);
    exporter.observeHistogram("mini.game.logic.queued_outputs", sample.queuedOutputs);
    exporter.observeHistogram("mini.game.logic.dropped_outputs", sample.droppedOutputs);
    exporter.observeHistogram("mini.game.logic.output_bytes", sample.outputBytes);
    observeDurationMs(exporter, "mini.game.logic.oldest_lag_ms", sample.oldestLag);
    observeDurationMs(exporter, "mini.game.logic.tick_duration_ms", sample.tickDuration);
    observeDurationMs(exporter, "mini.game.logic.tick_jitter_ms", sample.tickJitter);
    observeDurationMs(exporter, "mini.game.logic.output_queue_latency_ms", sample.outputQueueLatency);
}

void MetricsHookRecorder::record(MetricsExporter& exporter,
                                 const mini::game::GamePipelineMetricSample& sample) {
    exporter.incrementCounter(counterName("mini.game.pipeline", eventName(sample.event)));
    exporter.observeHistogram("mini.game.pipeline.frames_decoded", sample.framesDecoded);
    exporter.observeHistogram("mini.game.pipeline.bytes_consumed", sample.bytesConsumed);
    exporter.observeHistogram("mini.game.pipeline.buffered_bytes", sample.bufferedBytes);
    exporter.observeHistogram("mini.game.pipeline.logic_backlog", sample.logicBacklog);
    if (sample.continuationScheduled) {
        exporter.incrementCounter("mini.game.pipeline.continuation_scheduled");
    }
    if (sample.logicSubmitted) {
        exporter.incrementCounter("mini.game.pipeline.logic_submitted");
    }
    observeDurationMs(exporter, "mini.game.pipeline.batch_duration_ms", sample.batchDuration);
}

void MetricsHookRecorder::record(MetricsExporter& exporter,
                                 const mini::game::SessionMetricSample& sample) {
    exporter.incrementCounter(counterName("mini.game.session", eventName(sample.event)));
    if (sample.success) {
        exporter.incrementCounter("mini.game.session.success");
    }
    exporter.observeHistogram("mini.game.session.pending_events", sample.pendingEvents);
    exporter.observeHistogram("mini.game.session.drained_events", sample.drainedEvents);
    observeDurationMs(exporter, "mini.game.session.reconnect_duration_ms", sample.reconnectDuration);
    observeDurationMs(exporter, "mini.game.session.oldest_event_lag_ms", sample.oldestEventLag);
}

void MetricsHookRecorder::record(MetricsExporter& exporter,
                                 const mini::game::GameBackpressureMetricSample& sample) {
    exporter.incrementCounter(counterName("mini.game.backpressure", eventName(sample.event)));
    exporter.observeHistogram("mini.game.backpressure.current_value", sample.currentValue);
    exporter.observeHistogram("mini.game.backpressure.soft_limit", sample.softLimit);
    exporter.observeHistogram("mini.game.backpressure.hard_limit", sample.hardLimit);
    exporter.observeHistogram("mini.game.backpressure.backlog", sample.backlog);
    exporter.observeHistogram("mini.game.backpressure.fanout_connections", sample.fanoutConnections);
    exporter.observeHistogram("mini.game.backpressure.payload_bytes", sample.payloadBytes);
    exporter.observeHistogram("mini.game.backpressure.priority", sample.priority);
    observeDurationMs(exporter, "mini.game.backpressure.queue_latency_ms", sample.queueLatency);
}

void MetricsHookRecorder::record(MetricsExporter& exporter,
                                 const mini::game::GameSecurityMetricSample& sample) {
    exporter.incrementCounter(counterName("mini.game.security", eventName(sample.event)));
    if (sample.reason != mini::game::GameSecurityReason::None) {
        exporter.incrementCounter(counterName("mini.game.security.reason", reasonName(sample.reason)));
    }
    exporter.observeHistogram("mini.game.security.payload_bytes", sample.payloadBytes);
    exporter.observeHistogram("mini.game.security.current_value", sample.currentValue);
    exporter.observeHistogram("mini.game.security.limit", sample.limit);
}

}  // namespace mini::base
