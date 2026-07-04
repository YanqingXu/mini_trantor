#pragma once

// GameCommandQueue —— 逻辑命令入队通道。
// 输入命令只在此队列内排序，并由 LogicLoop 在固定步长中批量 drain。

#include "mini/base/Timestamp.h"
#include "mini/game/GameBackpressurePolicy.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/transport/ITransport.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mini::game::logic {

struct GameCommand {
    using TimePoint = mini::base::Timestamp;

    std::string sessionId;
    mini::net::transport::TransportSessionId transportSessionId{
        mini::net::transport::kInvalidTransportSessionId};
    std::weak_ptr<mini::net::TcpConnection> sourceConnection;
    std::weak_ptr<mini::net::transport::ITransportEndpoint> sourceTransport;
    std::string payload;
    std::uint32_t priority{toMetricPriority(GameMessagePriority::Normal)};
    TimePoint enqueuedAt;

    GameCommand() = default;

    GameCommand(std::string sessionId_,
                std::weak_ptr<mini::net::TcpConnection> sourceConnection_,
                std::string payload_,
                std::uint32_t priority_ = toMetricPriority(GameMessagePriority::Normal))
        : sessionId(std::move(sessionId_)),
          sourceConnection(std::move(sourceConnection_)),
          payload(std::move(payload_)),
          priority(priority_),
          enqueuedAt(mini::base::now()) {
    }

    GameCommand(std::string sessionId_,
                mini::net::transport::TransportSessionId transportSessionId_,
                std::weak_ptr<mini::net::transport::ITransportEndpoint> sourceTransport_,
                std::string payload_,
                std::uint32_t priority_ = toMetricPriority(GameMessagePriority::Normal))
        : sessionId(std::move(sessionId_)),
          transportSessionId(transportSessionId_),
          sourceTransport(std::move(sourceTransport_)),
          payload(std::move(payload_)),
          priority(priority_),
          enqueuedAt(mini::base::now()) {
    }
};

using GameCommandPtr = std::shared_ptr<GameCommand>;
using GameCommandBatch = std::vector<GameCommandPtr>;

class GameCommandQueue {
public:
    struct AdmissionResult {
        enum class Status {
            Accepted,
            RejectedInvalidCommand,
            RejectedHardBacklog,
            RejectedHardOldestLag,
        };

        Status status{Status::RejectedInvalidCommand};
        std::size_t backlog{0};
        std::chrono::milliseconds oldestLag{std::chrono::milliseconds::zero()};
        std::size_t hardBacklog{0};
        std::chrono::milliseconds hardOldestLag{std::chrono::milliseconds::zero()};

        bool accepted() const noexcept {
            return status == Status::Accepted;
        }
    };

    void enqueue(GameCommandPtr command);
    void enqueue(GameCommand command);
    AdmissionResult tryEnqueue(GameCommandPtr command,
                               std::size_t hardBacklog,
                               std::chrono::milliseconds hardOldestLag);

    GameCommandBatch drain(std::size_t maxCommands);
    std::size_t size() const;
    std::chrono::milliseconds oldestLag() const;
    void clear();

private:
    std::deque<GameCommandPtr> queue_;
    mutable std::mutex mutex_;
};

}  // namespace mini::game::logic
