#pragma once

// LogicLoop —— 逻辑线程化入口。
// 目标：将高频 I/O 消息解耦到独占 loop，按 fixed-step 执行，避免阻塞 I/O loop。

#include "mini/base/MetricsHook.h"
#include "mini/base/Timestamp.h"
#include "mini/base/noncopyable.h"
#include "mini/game/logic/GameCommandQueue.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/TimerId.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mini::game::logic {

class LogicLoop : private mini::base::noncopyable {
public:
    using TickDuration = std::chrono::steady_clock::duration;
    using CommandProcessor = std::function<void(const GameCommand&, std::vector<GameCommand>&)>;
    using OutputDispatcher = std::function<void(std::vector<GameCommand>&&)>;

    struct Options {
        TickDuration fixedStep{std::chrono::milliseconds(16)};
        std::size_t maxCommandsPerTick{128};
    };

    LogicLoop();
    explicit LogicLoop(Options options);
    ~LogicLoop();

    void setProcessor(CommandProcessor processor);
    void setOutputDispatcher(OutputDispatcher dispatcher);
    void setMetricCallback(LogicLoopMetricCallback callback);

    bool submit(std::string sessionId,
                std::weak_ptr<mini::net::TcpConnection> sourceConnection,
                std::string payload);
    bool submit(GameCommandPtr command);

    void start();
    void stop();
    bool isRunning() const noexcept;

    std::size_t backlog() const;
    std::chrono::milliseconds oldestLag() const;
    std::size_t processedCount() const;

private:
    void onLogicTick();
    void dispatchOutputs(std::vector<GameCommand>&& outputs);
    CommandProcessor resolveProcessor() const;
    OutputDispatcher resolveOutputDispatcher() const;
    LogicLoopMetricCallback resolveMetricCallback() const;

    static void defaultOutputDispatch(std::vector<GameCommand>&& outputs);

    mini::net::EventLoopThread logicThread_;
    std::shared_ptr<GameCommandQueue> queue_;
    TickDuration fixedStep_;
    std::size_t maxCommandsPerTick_;
    std::atomic<bool> running_;
    mini::net::TimerId tickTimerId_;
    std::atomic<mini::net::EventLoop*> logicLoop_;
    std::atomic<std::size_t> processedCount_{0};
    mini::base::Timestamp lastTickAt_{};

    mutable std::mutex stateMutex_;
    CommandProcessor processor_;
    OutputDispatcher outputDispatcher_;
    LogicLoopMetricCallback metricCallback_;
};

}  // namespace mini::game::logic
