#include "mini/net/broadcast/BroadcastDispatcher.h"

#include "mini/net/EventLoop.h"
#include "mini/net/TcpConnection.h"

#include <utility>

namespace mini::net::broadcast {

BroadcastDispatcher::BroadcastDispatcher(EventLoop* baseLoop)
    : baseLoop_(baseLoop) {
}

void BroadcastDispatcher::dispatch(std::vector<BroadcastRouter::LoopBatch> batches,
                                 mini::net::buffer::PayloadPtr payload) {
    if (baseLoop_ == nullptr || !payload) {
        return;
    }
    if (!baseLoop_->isInLoopThread()) {
        baseLoop_->queueInLoop([this,
                                payload = std::move(payload),
                                batches = std::move(batches)]() mutable {
            dispatch(std::move(batches), payload);
        });
        return;
    }
    dispatchInLoop(std::move(batches), std::move(payload));
}

void BroadcastDispatcher::dispatchInLoop(std::vector<BroadcastRouter::LoopBatch> batches,
                                       mini::net::buffer::PayloadPtr payload) {
    baseLoop_->assertInLoopThread();
    if (batches.empty()) {
        return;
    }
    for (auto& batch : batches) {
        if (batch.loop == nullptr || batch.connections.empty()) {
            continue;
        }

        LoopBatchCommand command{std::move(batch.connections), payload};
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

    loop->queueInLoop([state = std::move(state)] {
        std::vector<LoopBatchCommand> batchCommands;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            batchCommands.swap(state->pending);
            state->scheduled = false;
        }
        for (const auto& command : batchCommands) {
            if (!command.payload || command.payload->empty()) {
                continue;
            }
            for (const auto& connection : command.connections) {
                if (connection && connection->connected()) {
                    connection->send(command.payload->view());
                }
            }
        }
    });
}

}  // namespace mini::net::broadcast
