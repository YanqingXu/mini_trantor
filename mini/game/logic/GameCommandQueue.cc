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

GameCommandQueue::AdmissionResult GameCommandQueue::tryEnqueue(
    GameCommandPtr command,
    std::size_t hardBacklog,
    std::chrono::milliseconds hardOldestLag) {
    AdmissionResult result;
    result.hardBacklog = hardBacklog;
    result.hardOldestLag = hardOldestLag;

    if (!command) {
        result.status = AdmissionResult::Status::RejectedInvalidCommand;
        return result;
    }

    ensureTimestamp(command->enqueuedAt);

    std::scoped_lock lock(mutex_);
    result.backlog = queue_.size();
    if (!queue_.empty()) {
        const auto& oldest = queue_.front();
        if (oldest && oldest->enqueuedAt != GameCommand::TimePoint{}) {
            result.oldestLag = std::chrono::duration_cast<std::chrono::milliseconds>(
                mini::base::now() - oldest->enqueuedAt);
        }
    }

    if (hardBacklog > 0 && queue_.size() >= hardBacklog) {
        result.status = AdmissionResult::Status::RejectedHardBacklog;
        return result;
    }
    if (hardOldestLag > std::chrono::milliseconds::zero() &&
        result.oldestLag >= hardOldestLag) {
        result.status = AdmissionResult::Status::RejectedHardOldestLag;
        return result;
    }

    queue_.emplace_back(std::move(command));
    result.status = AdmissionResult::Status::Accepted;
    result.backlog = queue_.size();
    return result;
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
