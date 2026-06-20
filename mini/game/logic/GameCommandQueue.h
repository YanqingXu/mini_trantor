#pragma once

// GameCommandQueue —— 逻辑命令入队通道。
// 输入命令只在此队列内排序，并由 LogicLoop 在固定步长中批量 drain。

#include "mini/base/Timestamp.h"
#include "mini/net/TcpConnection.h"

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
    std::weak_ptr<mini::net::TcpConnection> sourceConnection;
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
