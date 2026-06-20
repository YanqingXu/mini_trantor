#pragma once

// GameCommandQueue —— 逻辑命令入队通道。
// 输入命令只在此队列内排序，并由 LogicLoop 在固定步长中批量 drain。

#include "mini/base/Timestamp.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/transport/ITransport.h"

#include <chrono>
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
    TimePoint enqueuedAt;

    GameCommand() = default;

    GameCommand(std::string sessionId_,
                std::weak_ptr<mini::net::TcpConnection> sourceConnection_,
                std::string payload_)
        : sessionId(std::move(sessionId_)),
          sourceConnection(std::move(sourceConnection_)),
          payload(std::move(payload_)),
          enqueuedAt(mini::base::now()) {
    }

    GameCommand(std::string sessionId_,
                mini::net::transport::TransportSessionId transportSessionId_,
                std::weak_ptr<mini::net::transport::ITransportEndpoint> sourceTransport_,
                std::string payload_)
        : sessionId(std::move(sessionId_)),
          transportSessionId(transportSessionId_),
          sourceTransport(std::move(sourceTransport_)),
          payload(std::move(payload_)),
          enqueuedAt(mini::base::now()) {
    }
};

using GameCommandPtr = std::shared_ptr<GameCommand>;
using GameCommandBatch = std::vector<GameCommandPtr>;

class GameCommandQueue {
public:
    void enqueue(GameCommandPtr command);
    void enqueue(GameCommand command);

    GameCommandBatch drain(std::size_t maxCommands);
    std::size_t size() const;
    std::chrono::milliseconds oldestLag() const;
    void clear();

private:
    std::deque<GameCommandPtr> queue_;
    mutable std::mutex mutex_;
};

}  // namespace mini::game::logic
