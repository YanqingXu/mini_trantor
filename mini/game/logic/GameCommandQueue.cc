#include "mini/game/logic/GameCommandQueue.h"

#include <algorithm>

namespace mini::game::logic {

namespace {

void ensureTimestamp(GameCommand::TimePoint& when) {
    if (when == GameCommand::TimePoint{}) {
        when = mini::base::now();
    }
}

}  // namespace

void GameCommandQueue::enqueue(GameCommandPtr command) {
    if (!command) {
        return;
    }

    ensureTimestamp(command->enqueuedAt);

    std::scoped_lock lock(mutex_);
    queue_.emplace_back(std::move(command));
}

void GameCommandQueue::enqueue(GameCommand command) {
    auto owned = std::make_shared<GameCommand>(std::move(command));
    enqueue(std::move(owned));
}

GameCommandBatch GameCommandQueue::drain(std::size_t maxCommands) {
    GameCommandBatch commands;
    if (maxCommands == 0) {
        return commands;
    }

    std::scoped_lock lock(mutex_);
    commands.reserve(std::min(maxCommands, queue_.size()));

    for (std::size_t i = 0; i < maxCommands && !queue_.empty(); ++i) {
        commands.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    return commands;
}

std::size_t GameCommandQueue::size() const {
    std::scoped_lock lock(mutex_);
    return queue_.size();
}

std::chrono::milliseconds GameCommandQueue::oldestLag() const {
    std::scoped_lock lock(mutex_);
    if (queue_.empty()) {
        return std::chrono::milliseconds::zero();
    }

    const auto& command = queue_.front();
    if (!command || command->enqueuedAt == GameCommand::TimePoint{}) {
        return std::chrono::milliseconds::zero();
    }

    return std::chrono::duration_cast<std::chrono::milliseconds>(
        mini::base::now() - command->enqueuedAt);
}

void GameCommandQueue::clear() {
    std::scoped_lock lock(mutex_);
    queue_.clear();
}

}  // namespace mini::game::logic
