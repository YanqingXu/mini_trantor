#include "mini/game/logic/LogicLoop.h"

#include <mutex>
#include <utility>

namespace mini::game::logic {

LogicLoop::LogicLoop()
    : LogicLoop(Options{}) {}

LogicLoop::LogicLoop(Options options)
    : logicThread_([this](mini::net::EventLoop* loop) { logicLoop_.store(loop); }),
      queue_(std::make_shared<GameCommandQueue>()),
      fixedStep_(options.fixedStep),
      maxCommandsPerTick_(options.maxCommandsPerTick),
      running_(false),
      tickTimerId_(),
      logicLoop_(nullptr),
      outputDispatcher_(defaultOutputDispatch) {
    if (fixedStep_ <= TickDuration::zero()) {
        fixedStep_ = std::chrono::milliseconds(16);
    }
    if (maxCommandsPerTick_ == 0) {
        maxCommandsPerTick_ = 1;
    }
}

LogicLoop::~LogicLoop() {
    stop();
}

void LogicLoop::setProcessor(CommandProcessor processor) {
    std::scoped_lock lock(stateMutex_);
    processor_ = std::move(processor);
}

void LogicLoop::setOutputDispatcher(OutputDispatcher dispatcher) {
    std::scoped_lock lock(stateMutex_);
    if (dispatcher) {
        outputDispatcher_ = std::move(dispatcher);
    } else {
        outputDispatcher_ = defaultOutputDispatch;
    }
}

bool LogicLoop::submit(std::string sessionId,
                       std::weak_ptr<mini::net::TcpConnection> sourceConnection,
                       std::string payload) {
    auto command = std::make_shared<GameCommand>(std::move(sessionId),
                                                 std::move(sourceConnection),
                                                 std::move(payload));
    return submit(command);
}

bool LogicLoop::submit(GameCommandPtr command) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    if (!command) {
        return false;
    }

    queue_->enqueue(std::move(command));
    return true;
}

void LogicLoop::start() {
    if (running_.exchange(true)) {
        return;
    }

    auto* loop = logicThread_.startLoop();
    if (!loop) {
        running_.store(false);
        return;
    }
    logicLoop_.store(loop, std::memory_order_release);

    loop->runInLoop([this, loop] {
        tickTimerId_ = loop->runEvery(fixedStep_, [this] { onLogicTick(); });
    });
}

void LogicLoop::stop() {
    auto expectedRunning = true;
    if (!running_.compare_exchange_strong(expectedRunning, false)) {
        return;
    }

    auto* loop = logicLoop_.load(std::memory_order_acquire);
    if (!loop) {
        return;
    }

    loop->runInLoop([this, loop] {
        if (tickTimerId_.valid()) {
            loop->cancel(tickTimerId_);
        }
    });
    loop->quit();
    logicLoop_.store(nullptr, std::memory_order_release);
    queue_->clear();
}

bool LogicLoop::isRunning() const noexcept {
    return running_.load();
}

std::size_t LogicLoop::backlog() const {
    return queue_->size();
}

std::chrono::milliseconds LogicLoop::oldestLag() const {
    return queue_->oldestLag();
}

std::size_t LogicLoop::processedCount() const {
    return processedCount_.load();
}

LogicLoop::CommandProcessor LogicLoop::resolveProcessor() const {
    std::scoped_lock lock(stateMutex_);
    return processor_;
}

LogicLoop::OutputDispatcher LogicLoop::resolveOutputDispatcher() const {
    std::scoped_lock lock(stateMutex_);
    return outputDispatcher_;
}

void LogicLoop::onLogicTick() {
    const auto commands = queue_->drain(maxCommandsPerTick_);
    if (commands.empty()) {
        return;
    }

    auto processor = resolveProcessor();
    std::vector<GameCommand> output;
    for (const auto& command : commands) {
        if (!command) {
            continue;
        }
        if (processor) {
            processor(*command, output);
        }
    }

    if (!output.empty()) {
        dispatchOutputs(std::move(output));
    }

    processedCount_.fetch_add(commands.size());
}

void LogicLoop::dispatchOutputs(std::vector<GameCommand>&& outputs) {
    auto dispatcher = resolveOutputDispatcher();
    if (!dispatcher) {
        return;
    }
    dispatcher(std::move(outputs));
}

void LogicLoop::defaultOutputDispatch(std::vector<GameCommand>&& outputs) {
    for (auto& output : outputs) {
        auto connection = output.sourceConnection.lock();
        if (!connection) {
            continue;
        }

        auto* connectionLoop = connection->getLoop();
        if (!connectionLoop) {
            continue;
        }

        auto payload = std::move(output.payload);
        connectionLoop->queueInLoop([connection, payload = std::move(payload)]() mutable {
            connection->send(payload);
        });
    }
}

}  // namespace mini::game::logic
