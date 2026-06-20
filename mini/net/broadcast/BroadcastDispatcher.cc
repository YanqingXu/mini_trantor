#include "mini/net/broadcast/BroadcastDispatcher.h"

#include "mini/net/EventLoop.h"
#include "mini/net/TcpConnection.h"

#include <utility>

namespace mini::net::broadcast {

BroadcastDispatcher::BroadcastDispatcher(EventLoop* baseLoop)
    : baseLoop_(baseLoop) {
}

void BroadcastDispatcher::setBroadcastMetricCallback(BroadcastMetricCallback cb) {
    broadcastMetricCallback_ = std::move(cb);
}

void BroadcastDispatcher::dispatch(std::vector<BroadcastRouter::LoopBatch> batches,
                                 mini::net::buffer::PayloadPtr payload) {
    DispatchMetricContext metrics;
    const auto now = mini::base::now();
    metrics.requestedAt = now;
    metrics.routedAt = now;
    metrics.payloadBytes = payload ? payload->size() : 0;
    dispatch(std::move(batches), std::move(payload), metrics);
}

void BroadcastDispatcher::dispatch(std::vector<BroadcastRouter::LoopBatch> batches,
                                 mini::net::buffer::PayloadPtr payload,
                                 DispatchMetricContext metrics) {
    if (baseLoop_ == nullptr || !payload) {
        return;
    }
    if (!baseLoop_->isInLoopThread()) {
        baseLoop_->queueInLoop([this,
                                payload = std::move(payload),
                                batches = std::move(batches),
                                metrics]() mutable {
            dispatch(std::move(batches), payload, metrics);
        });
        return;
    }
    dispatchInLoop(std::move(batches), std::move(payload), metrics);
}

void BroadcastDispatcher::dispatchInLoop(std::vector<BroadcastRouter::LoopBatch> batches,
                                       mini::net::buffer::PayloadPtr payload,
                                       DispatchMetricContext metrics) {
    baseLoop_->assertInLoopThread();
    if (batches.empty()) {
        return;
    }
    if (metrics.requestedAt == mini::base::Timestamp{}) {
        metrics.requestedAt = mini::base::now();
    }
    if (metrics.routedAt == mini::base::Timestamp{}) {
        metrics.routedAt = mini::base::now();
    }
    if (metrics.loopBatches == 0) {
        metrics.loopBatches = batches.size();
    }
    if (metrics.payloadBytes == 0 && payload) {
        metrics.payloadBytes = payload->size();
    }
    if (metrics.fanoutConnections == 0) {
        for (const auto& batch : batches) {
            metrics.fanoutConnections += batch.connections.size();
        }
    }

    BroadcastMetricSample routed;
    routed.event = BroadcastMetricEvent::Routed;
    routed.loop = baseLoop_;
    routed.targeted = metrics.targeted;
    routed.requestedSessions = metrics.requestedSessions;
    routed.loopBatches = metrics.loopBatches;
    routed.fanoutConnections = metrics.fanoutConnections;
    routed.payloadBytes = metrics.payloadBytes;
    routed.routeLatency = metrics.routeLatency;
    routed.fanoutLatency = mini::base::now() - metrics.requestedAt;
    emitBroadcastMetric(routed);

    for (auto& batch : batches) {
        if (batch.loop == nullptr || batch.connections.empty()) {
            continue;
        }

        LoopBatchCommand command{std::move(batch.connections), payload, metrics, mini::base::Timestamp{}};
        enqueueBatch(std::move(command));
    }
}

void BroadcastDispatcher::enqueueBatch(LoopBatchCommand command) {
    baseLoop_->assertInLoopThread();

    EventLoop* loop = nullptr;
    auto it = command.connections.begin();
    if (it != command.connections.end()) {
        loop = (*it)->getLoop();
    }
    if (!loop) {
        return;
    }

    auto state = [&] {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pendingByLoop_.find(loop);
        if (it == pendingByLoop_.end()) {
            auto inserted = pendingByLoop_.emplace(loop, std::make_shared<LoopState>());
            return inserted.first->second;
        }
        return it->second;
    }();

    bool scheduleFlush = false;
    const auto queuedAt = mini::base::now();
    command.queuedAt = queuedAt;
    auto metricsCallback = broadcastMetricCallback_;
    {
        std::lock_guard<std::mutex> stateLock(state->mutex);
        state->pending.push_back(std::move(command));
        if (!state->scheduled) {
            state->scheduled = true;
            scheduleFlush = true;
        }
    }
    if (!scheduleFlush) {
        return;
    }

    loop->queueInLoop([state = std::move(state), metricsCallback] {
        std::vector<LoopBatchCommand> batchCommands;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            batchCommands.swap(state->pending);
            state->scheduled = false;
        }
        const auto flushStartedAt = mini::base::now();
        for (const auto& command : batchCommands) {
            if (!command.payload || command.payload->empty()) {
                continue;
            }
            for (const auto& connection : command.connections) {
                if (connection && connection->connected()) {
                    connection->send(command.payload->view());
                }
            }
            if (metricsCallback) {
                const auto flushedAt = mini::base::now();
                BroadcastMetricSample sample;
                sample.event = BroadcastMetricEvent::LoopFlushed;
                sample.loop = command.connections.empty() ? nullptr : command.connections.front()->getLoop();
                sample.targeted = command.metrics.targeted;
                sample.requestedSessions = command.metrics.requestedSessions;
                sample.loopBatches = command.metrics.loopBatches;
                sample.fanoutConnections = command.connections.size();
                sample.payloadBytes = command.metrics.payloadBytes;
                sample.routeLatency = command.metrics.routeLatency;
                sample.queueLatency = flushStartedAt - command.queuedAt;
                sample.fanoutLatency = flushedAt - command.metrics.requestedAt;
                metricsCallback(sample);
            }
        }
    });
}

void BroadcastDispatcher::emitBroadcastMetric(BroadcastMetricSample sample) const {
    if (!broadcastMetricCallback_) {
        return;
    }
    broadcastMetricCallback_(sample);
}

}  // namespace mini::net::broadcast
