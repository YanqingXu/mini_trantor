// BroadcastDispatcher — ioLoop 分桶广播批处理器（Task-05）。
//
// 目标：
// - 将同一 ioLoop 的广播消息聚合到单次 queueInLoop；
// - 在 ioLoop 内批量遍历连接发送，减少跨 loop 队列入列次数；
// - 所有消息采用共享 payload 引用，降低复制成本。

#pragma once

#include "mini/base/MetricsHook.h"
#include "mini/base/Timestamp.h"
#include "mini/base/noncopyable.h"
#include "mini/codec/CodecAdapter.h"
#include "mini/net/buffer/Payload.h"
#include "mini/net/broadcast/BroadcastRouter.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mini::net {

class EventLoop;

namespace broadcast {

class BroadcastDispatcher : private mini::base::noncopyable {
public:
    struct DispatchMetricContext {
        mini::base::Timestamp requestedAt{};
        mini::base::Timestamp routedAt{};
        bool targeted{false};
        std::size_t requestedSessions{0};
        std::size_t loopBatches{0};
        std::size_t fanoutConnections{0};
        std::size_t payloadBytes{0};
        std::chrono::steady_clock::duration routeLatency{
            std::chrono::steady_clock::duration::zero()};
    };

    explicit BroadcastDispatcher(EventLoop* baseLoop);

    void setBroadcastMetricCallback(BroadcastMetricCallback cb);

    void dispatch(std::vector<BroadcastRouter::LoopBatch> batches,
                  mini::net::buffer::PayloadPtr payload);
    void dispatch(std::vector<BroadcastRouter::LoopBatch> batches,
                  mini::net::buffer::PayloadPtr payload,
                  DispatchMetricContext metrics);

    template <typename MessageT>
    void dispatch(std::vector<BroadcastRouter::LoopBatch> batches,
                  const MessageT& message,
                  const mini::codec::CodecAdapter& codec,
                  std::string* error = nullptr) {
        std::string payload;
        if (!codec.encode(&message, &payload, error)) {
            return;
        }
        dispatch(std::move(batches), std::make_shared<mini::net::buffer::Payload>(std::move(payload)));
    }

private:
    struct LoopBatchCommand {
        std::vector<TcpConnectionPtr> connections;
        mini::net::buffer::PayloadPtr payload;
        DispatchMetricContext metrics;
        mini::base::Timestamp queuedAt{};
    };

    struct LoopState {
        bool scheduled{false};
        std::vector<LoopBatchCommand> pending;
        mutable std::mutex mutex;
    };

    void dispatchInLoop(std::vector<BroadcastRouter::LoopBatch> batches,
                        mini::net::buffer::PayloadPtr payload,
                        DispatchMetricContext metrics);
    void enqueueBatch(LoopBatchCommand command);
    void emitBroadcastMetric(BroadcastMetricSample sample) const;

    EventLoop* baseLoop_{nullptr};
    BroadcastMetricCallback broadcastMetricCallback_;
    mutable std::mutex mutex_;
    std::unordered_map<EventLoop*, std::shared_ptr<LoopState>> pendingByLoop_;
};

}  // namespace broadcast

}  // namespace mini::net
