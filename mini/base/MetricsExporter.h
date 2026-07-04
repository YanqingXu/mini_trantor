#pragma once

// MetricsExporter 是 MetricsHook 的可选聚合层。
// Hook 仍在事件 owner loop 上触发；exporter 只记录观测值，不改变 reactor 行为。

#include "mini/base/MetricsHook.h"
#include "mini/base/noncopyable.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mini::base {

struct CounterSnapshot {
    std::string name;
    std::uint64_t value{0};
};

struct HistogramSnapshot {
    std::string name;
    std::uint64_t count{0};
    double sum{0.0};
    double min{0.0};
    double max{0.0};

    double average() const noexcept {
        return count == 0 ? 0.0 : sum / static_cast<double>(count);
    }
};

struct MetricsSnapshot {
    std::vector<CounterSnapshot> counters;
    std::vector<HistogramSnapshot> histograms;
};

struct MetricLabel {
    std::string key;
    std::string value;
};

using MetricLabels = std::vector<MetricLabel>;

class MetricsExporter : private noncopyable {
public:
    virtual ~MetricsExporter() = default;

    virtual void incrementCounter(std::string_view name, std::uint64_t delta = 1) = 0;
    virtual void observeHistogram(std::string_view name, double value) = 0;
};

class InMemoryMetricsExporter final : public MetricsExporter {
public:
    void incrementCounter(std::string_view name, std::uint64_t delta = 1) override;
    void observeHistogram(std::string_view name, double value) override;

    std::uint64_t counterValue(std::string_view name) const;
    HistogramSnapshot histogram(std::string_view name) const;
    MetricsSnapshot snapshot() const;
    void reset();

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::uint64_t> counters_;
    std::unordered_map<std::string, HistogramSnapshot> histograms_;
};

class TaggedMetricsExporter final : public MetricsExporter {
public:
    TaggedMetricsExporter(std::shared_ptr<MetricsExporter> sink, MetricLabels labels);

    void incrementCounter(std::string_view name, std::uint64_t delta = 1) override;
    void observeHistogram(std::string_view name, double value) override;

    const MetricLabels& labels() const noexcept { return labels_; }

private:
    std::shared_ptr<MetricsExporter> sink_;
    MetricLabels labels_;
    std::string labelSuffix_;
};

std::string metricNameWithLabels(std::string_view name, const MetricLabels& labels);
std::string renderPrometheusText(const MetricsSnapshot& snapshot);

class MetricsHookRecorder {
public:
    explicit MetricsHookRecorder(std::shared_ptr<MetricsExporter> exporter);

    mini::net::ConnectionEventCallback makeConnectionEventCallback() const;
    mini::net::BackpressureEventCallback makeBackpressureEventCallback() const;
    mini::net::ConnectorEventCallback makeConnectorEventCallback() const;
    mini::net::TlsEventCallback makeTlsEventCallback() const;
    mini::net::BroadcastMetricCallback makeBroadcastCallback() const;
    mini::net::EventLoopMetricCallback makeEventLoopCallback() const;
    mini::net::UdpMetricCallback makeUdpCallback() const;
    mini::game::logic::LogicLoopMetricCallback makeLogicLoopCallback() const;
    mini::game::GamePipelineMetricCallback makeGamePipelineCallback() const;
    mini::game::SessionMetricCallback makeSessionCallback() const;
    mini::game::GameBackpressureMetricCallback makeGameBackpressureCallback() const;
    mini::game::GameSecurityMetricCallback makeGameSecurityCallback() const;

    static void record(MetricsExporter& exporter, mini::net::ConnectionEvent event);
    static void record(MetricsExporter& exporter,
                       mini::net::BackpressureEvent event,
                       std::size_t bufferedBytes);
    static void record(MetricsExporter& exporter, mini::net::ConnectorEvent event);
    static void record(MetricsExporter& exporter, mini::net::TlsEvent event);
    static void record(MetricsExporter& exporter, const mini::net::BroadcastMetricSample& sample);
    static void record(MetricsExporter& exporter, const mini::net::EventLoopMetricSample& sample);
    static void record(MetricsExporter& exporter, const mini::net::UdpMetricSample& sample);
    static void record(MetricsExporter& exporter, const mini::game::logic::LogicLoopMetricSample& sample);
    static void record(MetricsExporter& exporter, const mini::game::GamePipelineMetricSample& sample);
    static void record(MetricsExporter& exporter, const mini::game::SessionMetricSample& sample);
    static void record(MetricsExporter& exporter, const mini::game::GameBackpressureMetricSample& sample);
    static void record(MetricsExporter& exporter, const mini::game::GameSecurityMetricSample& sample);

private:
    std::shared_ptr<MetricsExporter> exporter_;
};

}  // namespace mini::base
