# Module Intent: MetricsExporter

## Intent

`MetricsExporter` is the optional aggregation layer for `MetricsHook`.
It turns typed hook samples and control events into stable counter / histogram observations
without changing where hooks fire or how reactor state is owned.

This module exists so benchmarks, CI, and future production adapters can consume metrics
through a replaceable exporter interface instead of hand-written counters in each test.

## Responsibilities

- Define a small `MetricsExporter` interface:
  - increment named counters
  - observe named histogram values
- Provide `InMemoryMetricsExporter` for tests, benchmarks, and local diagnostics.
- Provide `TaggedMetricsExporter` for static deployment labels without changing hook emission.
- Provide dependency-free Prometheus text rendering from value snapshots.
- Provide `MetricsHookRecorder` adapters from existing hook callback types to exporter calls.
- Keep metric names stable and human-readable.
- Allow callbacks returned by `MetricsHookRecorder` to outlive the recorder object by capturing
  the exporter ownership explicitly.

## Non-Responsibilities

- No global metrics registry.
- No background thread.
- No network push/pull exporter.
- No Prometheus/OpenTelemetry dependency in v1.
- No dynamic per-event label extraction from reactor/game object ownership.
- No behavior decisions based on metric values.
- No ownership of `EventLoop`, `TcpConnection`, session, or game objects.

## Invariants

1. Hook firing thread does not change: callbacks still run where the original module emits them.
2. Recording a metric must not call back into reactor modules.
3. `InMemoryMetricsExporter` must be thread-safe because different owner loops may record concurrently.
4. `MetricsHookRecorder` must reject a null exporter.
5. Exporter callbacks must hold only exporter ownership, not recorder or reactor object ownership.
6. Missing counters / histograms read as zero snapshots.
7. `TaggedMetricsExporter` must hold only sink exporter ownership and immutable labels.
8. Label encoding must be deterministic and safe for text export.
9. Prometheus text rendering must be a snapshot serialization step, not work done in hook callbacks.

## Threading Rules

- `MetricsExporter` has no owner `EventLoop`.
- `InMemoryMetricsExporter` protects mutable maps with an internal mutex.
- `TaggedMetricsExporter` has no mutable shared state beyond the sink exporter it wraps.
- Hook callbacks remain synchronous and lightweight; they should only record simple observations.
- If future exporters perform blocking I/O, they must do so outside owner loop callbacks.
- Prometheus text rendering should be called from snapshot/reporting code, not from hot owner-loop hooks.

## Ownership Rules

- Callers own exporter instances, usually through `std::shared_ptr<MetricsExporter>`.
- `MetricsHookRecorder` borrows no reactor objects and stores only a shared exporter pointer.
- Callback factories capture the exporter shared ownership directly.
- `TaggedMetricsExporter` owns its static label vector and shares ownership of the wrapped sink.
- Exporter snapshots are value copies.

## Failure Semantics

- Constructing `MetricsHookRecorder` with null exporter throws `std::invalid_argument`.
- Constructing `TaggedMetricsExporter` with null sink throws `std::invalid_argument`.
- Invalid metric names, invalid label keys, or duplicate label keys throw `std::invalid_argument`.
- Unknown enum values map to an `unknown` counter suffix.
- `InMemoryMetricsExporter::reset()` clears all aggregate state.

## Extension Points

- Implement another `MetricsExporter` for production export.
- Extend labels beyond static deployment labels only through explicit sample fields and tests.
- Add percentile sketches later behind the same histogram observation method.
- Add network pull/push endpoints outside owner-loop hook callbacks.

## Test Contracts

- `tests/unit/metrics/test_metrics_exporter.cpp`
  - counter aggregation
  - histogram aggregation
  - reset
  - static label encoding and validation
  - Prometheus text rendering from snapshots
  - control hook and structured sample mapping
- `tests/contract/net/test_metrics_exporter_contract.cpp`
  - recorder callback lifetime
  - concurrent hook recording
  - concurrent tagged exporter recording
  - owner-loop EventLoop metric recording
- `tests/integration/benchmark/test_game_server_metrics_smoke.cpp`
  - end-to-end game metrics hook chain recorded through tagged `MetricsHookRecorder`
  - rendered snapshot contains tagged Prometheus text samples

## Review Checklist

- Does the change preserve hook owner-thread semantics?
- Does it avoid global mutable state?
- Does it avoid owning reactor/game objects?
- Are labels static value data rather than hidden object ownership?
- Is text export performed from snapshots rather than inside hot callbacks?
- Are new public callbacks covered by direct contract tests?
- Are metric callbacks still lightweight enough for owner loop execution?
