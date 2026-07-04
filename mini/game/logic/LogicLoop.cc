#include "mini/game/logic/LogicLoop.h"

#include <chrono>
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
      admissionOptions_(options.admission),
      outputOptions_(options.output),
      running_(false),
      tickTimerId_(),
      logicLoop_(nullptr) {
    options.validate();
    if (fixedStep_ <= TickDuration::zero()) {
        fixedStep_ = std::chrono::milliseconds(16);
    }
    if (maxCommandsPerTick_ == 0) {
        maxCommandsPerTick_ = 1;
    }
}

LogicLoop::~LogicLoop() {
    stop();
    lifetimeToken_.reset();
}

void LogicLoop::setProcessor(CommandProcessor processor) {
    std::scoped_lock lock(stateMutex_);
    processor_ = std::move(processor);
}

void LogicLoop::setOutputDispatcher(OutputDispatcher dispatcher) {
    std::scoped_lock lock(stateMutex_);
    outputDispatcher_ = std::move(dispatcher);
}

void LogicLoop::setMetricCallback(LogicLoopMetricCallback callback) {
    std::scoped_lock lock(stateMutex_);
    metricCallback_ = std::move(callback);
}

void LogicLoop::setBackpressureMetricCallback(GameBackpressureMetricCallback callback) {
    std::scoped_lock lock(stateMutex_);
    backpressureMetricCallback_ = std::move(callback);
}

void LogicLoop::setOutputBackpressurePolicy(GameBackpressureOptions::OutputSend options) {
    options.validate();
    std::scoped_lock lock(stateMutex_);
    outputOptions_ = options;
}

bool LogicLoop::submit(std::string sessionId,
                       std::weak_ptr<mini::net::TcpConnection> sourceConnection,
                       std::string payload,
                       std::uint32_t priority) {
    return submitWithResult(
        std::move(sessionId),
        std::move(sourceConnection),
        std::move(payload),
        priority).accepted;
}

bool LogicLoop::submit(std::string sessionId,
                       mini::net::transport::TransportSessionId transportSessionId,
                       std::weak_ptr<mini::net::transport::ITransportEndpoint> sourceTransport,
                       std::string payload,
                       std::uint32_t priority) {
    return submitWithResult(
        std::move(sessionId),
        transportSessionId,
        std::move(sourceTransport),
        std::move(payload),
        priority).accepted;
}

bool LogicLoop::submit(GameCommandPtr command) {
    return submitWithResult(std::move(command)).accepted;
}

LogicLoop::SubmitResult LogicLoop::submitWithResult(
    std::string sessionId,
    std::weak_ptr<mini::net::TcpConnection> sourceConnection,
    std::string payload,
    std::uint32_t priority) {
    auto command = std::make_shared<GameCommand>(std::move(sessionId),
                                                 std::move(sourceConnection),
                                                 std::move(payload),
                                                 priority);
    return submitWithResult(std::move(command));
}

LogicLoop::SubmitResult LogicLoop::submitWithResult(
    std::string sessionId,
    mini::net::transport::TransportSessionId transportSessionId,
    std::weak_ptr<mini::net::transport::ITransportEndpoint> sourceTransport,
    std::string payload,
    std::uint32_t priority) {
    auto command = std::make_shared<GameCommand>(std::move(sessionId),
                                                 transportSessionId,
                                                 std::move(sourceTransport),
                                                 std::move(payload),
                                                 priority);
    return submitWithResult(std::move(command));
}

LogicLoop::SubmitResult LogicLoop::submitWithResult(GameCommandPtr command) {
    SubmitResult result;
    if (!running_.load(std::memory_order_acquire)) {
        result.accepted = false;
        result.action = GameBackpressureAction::Reject;
        result.reason = GameBackpressureReason::LogicLoopNotRunning;
        return result;
    }
    if (!command) {
        result.accepted = false;
        result.action = GameBackpressureAction::Reject;
        result.reason = GameBackpressureReason::InvalidCommand;
        return result;
    }

    const auto hardOldestLag = std::chrono::duration_cast<std::chrono::milliseconds>(
        admissionOptions_.hardOldestLag);
    auto admission = queue_->tryEnqueue(
        command,
        admissionOptions_.hardBacklog,
        hardOldestLag);
    result = makeSubmitResult(admission);

    auto callback = resolveMetricCallback();
    auto* loop = logicLoop_.load(std::memory_order_acquire);
    if (result.accepted && callback && loop) {
        auto queue = queue_;
        loop->queueInLoop([loop, queue = std::move(queue), callback = std::move(callback)] {
            LogicLoopMetricSample sample;
            sample.event = LogicLoopMetricEvent::CommandEnqueued;
            sample.loop = loop;
            sample.backlog = queue->size();
            sample.oldestLag = queue->oldestLag();
            callback(sample);
        });
    }

    emitBackpressureMetric(result, command, resolveBackpressureMetricCallback());
    return result;
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

    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop->runInLoop([this, loop, lifetime] {
        if (!lifetime.lock()) {
            return;
        }
        tickTimerId_ = loop->runEvery(fixedStep_, [this, lifetime] {
            if (!lifetime.lock()) {
                return;
            }
            onLogicTick();
        });
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

    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop->runInLoop([this, loop, lifetime] {
        if (!lifetime.lock()) {
            return;
        }
        if (tickTimerId_.valid()) {
            loop->cancel(tickTimerId_);
        }
    });
    loop->quit();
    logicThread_.stop();
    logicLoop_.store(nullptr, std::memory_order_release);
    tickTimerId_ = {};
    lastTickAt_ = {};
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

LogicLoopMetricCallback LogicLoop::resolveMetricCallback() const {
    std::scoped_lock lock(stateMutex_);
    return metricCallback_;
}

GameBackpressureMetricCallback LogicLoop::resolveBackpressureMetricCallback() const {
    std::scoped_lock lock(stateMutex_);
    return backpressureMetricCallback_;
}

GameBackpressureOptions::OutputSend LogicLoop::resolveOutputBackpressurePolicy() const {
    std::scoped_lock lock(stateMutex_);
    return outputOptions_;
}

LogicLoop::SubmitResult LogicLoop::makeSubmitResult(
    const GameCommandQueue::AdmissionResult& admission) const {
    SubmitResult result;
    result.backlog = admission.backlog;
    result.oldestLag = admission.oldestLag;

    switch (admission.status) {
    case GameCommandQueue::AdmissionResult::Status::Accepted:
        result.accepted = true;
        result.action = GameBackpressureAction::Accept;
        if (admissionOptions_.softBacklog > 0 &&
            admission.backlog >= admissionOptions_.softBacklog) {
            result.reason = GameBackpressureReason::LogicBacklogSoftLimit;
            result.currentValue = admission.backlog;
        } else {
            const auto softOldestLag = std::chrono::duration_cast<std::chrono::milliseconds>(
                admissionOptions_.softOldestLag);
            if (softOldestLag > std::chrono::milliseconds::zero() &&
                admission.oldestLag >= softOldestLag) {
                result.reason = GameBackpressureReason::LogicOldestLagSoftLimit;
            } else {
                result.reason = GameBackpressureReason::None;
            }
        }
        break;
    case GameCommandQueue::AdmissionResult::Status::RejectedHardBacklog:
        result.accepted = false;
        result.action = GameBackpressureAction::Reject;
        result.reason = GameBackpressureReason::LogicBacklogHardLimit;
        result.hardLimit = admission.hardBacklog;
        result.softLimit = admissionOptions_.softBacklog;
        break;
    case GameCommandQueue::AdmissionResult::Status::RejectedHardOldestLag:
        result.accepted = false;
        result.action = GameBackpressureAction::Reject;
        result.reason = GameBackpressureReason::LogicOldestLagHardLimit;
        result.hardLimit = static_cast<std::size_t>(admission.hardOldestLag.count());
        result.softLimit = static_cast<std::size_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                admissionOptions_.softOldestLag).count());
        break;
    case GameCommandQueue::AdmissionResult::Status::RejectedInvalidCommand:
        result.accepted = false;
        result.action = GameBackpressureAction::Reject;
        result.reason = GameBackpressureReason::InvalidCommand;
        break;
    }

    if (result.reason == GameBackpressureReason::LogicBacklogSoftLimit ||
        result.reason == GameBackpressureReason::LogicBacklogHardLimit) {
        result.currentValue = admission.backlog;
        if (result.hardLimit == 0) {
            result.hardLimit = admissionOptions_.hardBacklog;
        }
        if (result.softLimit == 0) {
            result.softLimit = admissionOptions_.softBacklog;
        }
    } else if (result.reason == GameBackpressureReason::LogicOldestLagSoftLimit ||
               result.reason == GameBackpressureReason::LogicOldestLagHardLimit) {
        result.currentValue = static_cast<std::size_t>(admission.oldestLag.count());
        if (result.hardLimit == 0) {
            result.hardLimit = static_cast<std::size_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    admissionOptions_.hardOldestLag).count());
        }
        if (result.softLimit == 0) {
            result.softLimit = static_cast<std::size_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    admissionOptions_.softOldestLag).count());
        }
    }

    return result;
}

void LogicLoop::emitBackpressureMetric(const SubmitResult& result,
                                       const GameCommandPtr& command,
                                       GameBackpressureMetricCallback callback) {
    if (!callback) {
        return;
    }

    auto* loop = logicLoop_.load(std::memory_order_acquire);
    if (!loop) {
        return;
    }

    GameBackpressureMetricSample sample;
    sample.event = result.accepted
        ? GameBackpressureMetricEvent::LogicAccepted
        : GameBackpressureMetricEvent::LogicRejected;
    sample.layer = GameBackpressureLayer::LogicAdmission;
    sample.action = result.action;
    sample.reason = result.reason;
    sample.loop = loop;
    sample.backlog = result.backlog;
    sample.currentValue = result.currentValue;
    sample.softLimit = result.softLimit;
    sample.hardLimit = result.hardLimit;
    sample.queueLatency = result.oldestLag;
    if (command) {
        sample.sessionToken = command->sessionId;
        sample.transportSessionId = command->transportSessionId;
        sample.priority = command->priority;
        sample.payloadBytes = command->payload.size();
    }

    if (loop->isInLoopThread()) {
        callback(sample);
        return;
    }

    loop->queueInLoop([callback = std::move(callback), sample = std::move(sample)]() mutable {
        callback(sample);
    });
}

void LogicLoop::emitBackpressureMetric(GameBackpressureMetricSample sample,
                                       GameBackpressureMetricCallback callback) {
    if (!callback) {
        return;
    }
    callback(sample);
}

void LogicLoop::onLogicTick() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    const auto tickStartedAt = mini::base::now();
    auto tickJitter = TickDuration::zero();
    if (lastTickAt_ != mini::base::Timestamp{}) {
        const auto elapsed = tickStartedAt - lastTickAt_;
        tickJitter = elapsed >= fixedStep_ ? elapsed - fixedStep_ : fixedStep_ - elapsed;
    }
    lastTickAt_ = tickStartedAt;

    auto metricCallback = resolveMetricCallback();
    const auto commands = queue_->drain(maxCommandsPerTick_);
    if (commands.empty()) {
        if (metricCallback) {
            LogicLoopMetricSample sample;
            sample.event = LogicLoopMetricEvent::TickCompleted;
            sample.loop = logicLoop_.load(std::memory_order_acquire);
            sample.backlog = queue_->size();
            sample.oldestLag = queue_->oldestLag();
            sample.tickDuration = mini::base::now() - tickStartedAt;
            sample.tickJitter = tickJitter;
            metricCallback(sample);
        }
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

    if (metricCallback) {
        LogicLoopMetricSample sample;
        sample.event = LogicLoopMetricEvent::TickCompleted;
        sample.loop = logicLoop_.load(std::memory_order_acquire);
        sample.backlog = queue_->size();
        sample.drainedCommands = commands.size();
        sample.oldestLag = queue_->oldestLag();
        sample.tickDuration = mini::base::now() - tickStartedAt;
        sample.tickJitter = tickJitter;
        metricCallback(sample);
    }
}

void LogicLoop::dispatchOutputs(std::vector<GameCommand>&& outputs) {
    auto dispatcher = resolveOutputDispatcher();
    if (dispatcher) {
        dispatcher(std::move(outputs));
        return;
    }
    defaultOutputDispatch(
        std::move(outputs),
        resolveMetricCallback(),
        resolveBackpressureMetricCallback());
}

void LogicLoop::defaultOutputDispatch(std::vector<GameCommand>&& outputs,
                                      LogicLoopMetricCallback metricCallback,
                                      GameBackpressureMetricCallback backpressureMetricCallback) {
    const auto outputBatch = outputs.size();
    std::size_t queuedOutputs = 0;
    std::size_t droppedOutputs = 0;
    std::size_t outputBytes = 0;
    const auto outputOptions = resolveOutputBackpressurePolicy();
    auto* logicLoop = logicLoop_.load(std::memory_order_acquire);

    auto emitOutputSample = [&](GameBackpressureMetricEvent event,
                                GameBackpressureAction action,
                                GameBackpressureReason reason,
                                mini::net::EventLoop* loop,
                                const GameCommand& command,
                                std::size_t payloadBytes,
                                std::size_t currentValue,
                                std::size_t softLimit,
                                std::size_t hardLimit,
                                GameBackpressureMetricSample::Duration queueLatency =
                                    GameBackpressureMetricSample::Duration::zero()) {
        GameBackpressureMetricSample sample;
        sample.event = event;
        sample.layer = GameBackpressureLayer::OutputSend;
        sample.action = action;
        sample.reason = reason;
        sample.loop = loop;
        sample.sessionToken = command.sessionId;
        sample.transportSessionId = command.transportSessionId;
        sample.priority = command.priority;
        sample.currentValue = currentValue;
        sample.softLimit = softLimit;
        sample.hardLimit = hardLimit;
        sample.payloadBytes = payloadBytes;
        sample.queueLatency = queueLatency;
        emitBackpressureMetric(std::move(sample), backpressureMetricCallback);
    };

    auto shouldDropPayload = [&](const GameCommand& command, std::size_t payloadBytes) {
        if (outputOptions.hardQueuedBytes == 0 ||
            payloadBytes <= outputOptions.hardQueuedBytes) {
            return false;
        }
        ++droppedOutputs;
        emitOutputSample(
            GameBackpressureMetricEvent::OutputDropped,
            GameBackpressureAction::Reject,
            GameBackpressureReason::OutputQueuedBytesHardLimit,
            logicLoop,
            command,
            payloadBytes,
            payloadBytes,
            outputOptions.softQueuedBytes,
            outputOptions.hardQueuedBytes);
        return true;
    };

    auto shouldDropLowPriorityPayload = [&](const GameCommand& command, std::size_t payloadBytes) {
        const auto effectiveSoft = outputOptions.priority.effectiveSoftLimit(
            outputOptions.softQueuedBytes,
            outputOptions.hardQueuedBytes);
        if (!outputOptions.priority.shouldDrop(
                command.priority,
                payloadBytes,
                outputOptions.softQueuedBytes,
                outputOptions.hardQueuedBytes)) {
            return false;
        }

        ++droppedOutputs;
        emitOutputSample(
            GameBackpressureMetricEvent::OutputDropped,
            GameBackpressureAction::DropLowPriority,
            GameBackpressureReason::OutputQueuedBytesSoftLimit,
            logicLoop,
            command,
            payloadBytes,
            payloadBytes,
            effectiveSoft,
            outputOptions.hardQueuedBytes);
        return true;
    };

    for (auto& output : outputs) {
        if (auto endpoint = output.sourceTransport.lock()) {
            auto* endpointLoop = endpoint->getLoop();
            if (!endpointLoop) {
                ++droppedOutputs;
                emitOutputSample(
                    GameBackpressureMetricEvent::OutputDropped,
                    GameBackpressureAction::Reject,
                    GameBackpressureReason::EndpointExpired,
                    logicLoop,
                    output,
                    output.payload.size(),
                    0,
                    0,
                    0);
                continue;
            }

            const auto outputCreatedAt = output.enqueuedAt == mini::base::Timestamp{}
                ? mini::base::now()
                : output.enqueuedAt;
            auto payload = std::move(output.payload);
            const auto payloadBytes = payload.size();
            if (shouldDropPayload(output, payloadBytes)) {
                continue;
            }
            if (shouldDropLowPriorityPayload(output, payloadBytes)) {
                continue;
            }
            outputBytes += payloadBytes;
            ++queuedOutputs;
            endpointLoop->queueInLoop([endpoint,
                                       endpointLoop,
                                       payload = std::move(payload),
                                       outputCreatedAt,
                                       outputOptions,
                                       command = output,
                                       callback = metricCallback,
                                       bpCallback = backpressureMetricCallback]() mutable {
                const auto outputBytes = payload.size();
                const auto queueLatency = mini::base::now() - outputCreatedAt;
                if (outputOptions.hardQueueLatency > GameBackpressureMetricSample::Duration::zero() &&
                    queueLatency >= outputOptions.hardQueueLatency) {
                    if (bpCallback) {
                        GameBackpressureMetricSample sample;
                        sample.event = GameBackpressureMetricEvent::OutputDropped;
                        sample.layer = GameBackpressureLayer::OutputSend;
                        sample.action = GameBackpressureAction::Reject;
                        sample.reason = GameBackpressureReason::OutputQueueLatencyHardLimit;
                        sample.loop = endpointLoop;
                        sample.sessionToken = command.sessionId;
                        sample.transportSessionId = command.transportSessionId;
                        sample.priority = command.priority;
                        sample.currentValue = static_cast<std::size_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                queueLatency).count());
                        sample.softLimit = static_cast<std::size_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                outputOptions.softQueueLatency).count());
                        sample.hardLimit = static_cast<std::size_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                outputOptions.hardQueueLatency).count());
                        sample.payloadBytes = outputBytes;
                        sample.queueLatency = queueLatency;
                        bpCallback(sample);
                    }
                    return;
                }
                if (outputOptions.priority.shouldDrop(command.priority,
                                                      queueLatency,
                                                      outputOptions.softQueueLatency,
                                                      outputOptions.hardQueueLatency)) {
                    if (bpCallback) {
                        const auto effectiveSoft = outputOptions.priority.effectiveSoftLimit(
                            outputOptions.softQueueLatency,
                            outputOptions.hardQueueLatency);
                        GameBackpressureMetricSample sample;
                        sample.event = GameBackpressureMetricEvent::OutputDropped;
                        sample.layer = GameBackpressureLayer::OutputSend;
                        sample.action = GameBackpressureAction::DropLowPriority;
                        sample.reason = GameBackpressureReason::OutputQueueLatencySoftLimit;
                        sample.loop = endpointLoop;
                        sample.sessionToken = command.sessionId;
                        sample.transportSessionId = command.transportSessionId;
                        sample.priority = command.priority;
                        sample.currentValue = static_cast<std::size_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                queueLatency).count());
                        sample.softLimit = static_cast<std::size_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                effectiveSoft).count());
                        sample.hardLimit = static_cast<std::size_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                outputOptions.hardQueueLatency).count());
                        sample.payloadBytes = outputBytes;
                        sample.queueLatency = queueLatency;
                        bpCallback(sample);
                    }
                    return;
                }
                endpoint->send(payload);
                if (bpCallback) {
                    GameBackpressureMetricSample sample;
                    sample.event = GameBackpressureMetricEvent::OutputQueued;
                    sample.layer = GameBackpressureLayer::OutputSend;
                    sample.action = GameBackpressureAction::Accept;
                    sample.reason = GameBackpressureReason::None;
                    sample.loop = endpointLoop;
                    sample.sessionToken = command.sessionId;
                    sample.transportSessionId = command.transportSessionId;
                    sample.priority = command.priority;
                    sample.currentValue = outputBytes;
                    sample.softLimit = outputOptions.softQueuedBytes;
                    sample.hardLimit = outputOptions.hardQueuedBytes;
                    sample.payloadBytes = outputBytes;
                    sample.queueLatency = queueLatency;
                    const auto effectiveSoftLatency = outputOptions.priority.effectiveSoftLimit(
                        outputOptions.softQueueLatency,
                        outputOptions.hardQueueLatency);
                    if (effectiveSoftLatency > GameBackpressureMetricSample::Duration::zero() &&
                        queueLatency >= effectiveSoftLatency) {
                        sample.reason = GameBackpressureReason::OutputQueueLatencySoftLimit;
                        sample.currentValue = static_cast<std::size_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                queueLatency).count());
                        sample.softLimit = static_cast<std::size_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                effectiveSoftLatency).count());
                        sample.hardLimit = static_cast<std::size_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                outputOptions.hardQueueLatency).count());
                    } else {
                        const auto effectiveSoftBytes = outputOptions.priority.effectiveSoftLimit(
                            outputOptions.softQueuedBytes,
                            outputOptions.hardQueuedBytes);
                        if (effectiveSoftBytes > 0 && outputBytes >= effectiveSoftBytes) {
                            sample.reason = GameBackpressureReason::OutputQueuedBytesSoftLimit;
                            sample.softLimit = effectiveSoftBytes;
                        }
                    }
                    bpCallback(sample);
                }
                if (callback) {
                    LogicLoopMetricSample sample;
                    sample.event = LogicLoopMetricEvent::OutputSent;
                    sample.loop = endpointLoop;
                    sample.outputBatch = 1;
                    sample.queuedOutputs = 1;
                    sample.outputBytes = outputBytes;
                    sample.outputQueueLatency = queueLatency;
                    callback(sample);
                }
            });
            continue;
        }

        auto connection = output.sourceConnection.lock();
        if (!connection) {
            ++droppedOutputs;
            emitOutputSample(
                GameBackpressureMetricEvent::OutputDropped,
                GameBackpressureAction::Reject,
                GameBackpressureReason::EndpointExpired,
                logicLoop,
                output,
                output.payload.size(),
                0,
                0,
                0);
            continue;
        }

        auto* connectionLoop = connection->getLoop();
        if (!connectionLoop) {
            ++droppedOutputs;
            emitOutputSample(
                GameBackpressureMetricEvent::OutputDropped,
                GameBackpressureAction::Reject,
                GameBackpressureReason::EndpointExpired,
                logicLoop,
                output,
                output.payload.size(),
                0,
                0,
                0);
            continue;
        }

        const auto outputCreatedAt = output.enqueuedAt == mini::base::Timestamp{}
            ? mini::base::now()
            : output.enqueuedAt;
        auto payload = std::move(output.payload);
        const auto payloadBytes = payload.size();
        if (shouldDropPayload(output, payloadBytes)) {
            continue;
        }
        if (shouldDropLowPriorityPayload(output, payloadBytes)) {
            continue;
        }
        outputBytes += payloadBytes;
        ++queuedOutputs;
        connectionLoop->queueInLoop([connection,
                                     connectionLoop,
                                     payload = std::move(payload),
                                     outputCreatedAt,
                                     outputOptions,
                                     command = output,
                                     callback = metricCallback,
                                     bpCallback = backpressureMetricCallback]() mutable {
            const auto outputBytes = payload.size();
            const auto queueLatency = mini::base::now() - outputCreatedAt;
            if (outputOptions.hardQueueLatency > GameBackpressureMetricSample::Duration::zero() &&
                queueLatency >= outputOptions.hardQueueLatency) {
                if (bpCallback) {
                    GameBackpressureMetricSample sample;
                    sample.event = GameBackpressureMetricEvent::OutputDropped;
                    sample.layer = GameBackpressureLayer::OutputSend;
                    sample.action = GameBackpressureAction::Reject;
                    sample.reason = GameBackpressureReason::OutputQueueLatencyHardLimit;
                    sample.loop = connectionLoop;
                    sample.sessionToken = command.sessionId;
                    sample.transportSessionId = command.transportSessionId;
                    sample.priority = command.priority;
                    sample.currentValue = static_cast<std::size_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            queueLatency).count());
                    sample.softLimit = static_cast<std::size_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            outputOptions.softQueueLatency).count());
                    sample.hardLimit = static_cast<std::size_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            outputOptions.hardQueueLatency).count());
                    sample.payloadBytes = outputBytes;
                    sample.queueLatency = queueLatency;
                    bpCallback(sample);
                }
                return;
            }
            if (outputOptions.priority.shouldDrop(command.priority,
                                                  queueLatency,
                                                  outputOptions.softQueueLatency,
                                                  outputOptions.hardQueueLatency)) {
                if (bpCallback) {
                    const auto effectiveSoft = outputOptions.priority.effectiveSoftLimit(
                        outputOptions.softQueueLatency,
                        outputOptions.hardQueueLatency);
                    GameBackpressureMetricSample sample;
                    sample.event = GameBackpressureMetricEvent::OutputDropped;
                    sample.layer = GameBackpressureLayer::OutputSend;
                    sample.action = GameBackpressureAction::DropLowPriority;
                    sample.reason = GameBackpressureReason::OutputQueueLatencySoftLimit;
                    sample.loop = connectionLoop;
                    sample.sessionToken = command.sessionId;
                    sample.transportSessionId = command.transportSessionId;
                    sample.priority = command.priority;
                    sample.currentValue = static_cast<std::size_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            queueLatency).count());
                    sample.softLimit = static_cast<std::size_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            effectiveSoft).count());
                    sample.hardLimit = static_cast<std::size_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            outputOptions.hardQueueLatency).count());
                    sample.payloadBytes = outputBytes;
                    sample.queueLatency = queueLatency;
                    bpCallback(sample);
                }
                return;
            }
            connection->send(payload);
            if (bpCallback) {
                GameBackpressureMetricSample sample;
                sample.event = GameBackpressureMetricEvent::OutputQueued;
                sample.layer = GameBackpressureLayer::OutputSend;
                sample.action = GameBackpressureAction::Accept;
                sample.reason = GameBackpressureReason::None;
                sample.loop = connectionLoop;
                sample.sessionToken = command.sessionId;
                sample.transportSessionId = command.transportSessionId;
                sample.priority = command.priority;
                sample.currentValue = outputBytes;
                sample.softLimit = outputOptions.softQueuedBytes;
                sample.hardLimit = outputOptions.hardQueuedBytes;
                sample.payloadBytes = outputBytes;
                sample.queueLatency = queueLatency;
                const auto effectiveSoftLatency = outputOptions.priority.effectiveSoftLimit(
                    outputOptions.softQueueLatency,
                    outputOptions.hardQueueLatency);
                if (effectiveSoftLatency > GameBackpressureMetricSample::Duration::zero() &&
                    queueLatency >= effectiveSoftLatency) {
                    sample.reason = GameBackpressureReason::OutputQueueLatencySoftLimit;
                    sample.currentValue = static_cast<std::size_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            queueLatency).count());
                    sample.softLimit = static_cast<std::size_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            effectiveSoftLatency).count());
                    sample.hardLimit = static_cast<std::size_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            outputOptions.hardQueueLatency).count());
                } else {
                    const auto effectiveSoftBytes = outputOptions.priority.effectiveSoftLimit(
                        outputOptions.softQueuedBytes,
                        outputOptions.hardQueuedBytes);
                    if (effectiveSoftBytes > 0 && outputBytes >= effectiveSoftBytes) {
                        sample.reason = GameBackpressureReason::OutputQueuedBytesSoftLimit;
                        sample.softLimit = effectiveSoftBytes;
                    }
                }
                bpCallback(sample);
            }
            if (callback) {
                LogicLoopMetricSample sample;
                sample.event = LogicLoopMetricEvent::OutputSent;
                sample.loop = connectionLoop;
                sample.outputBatch = 1;
                sample.queuedOutputs = 1;
                sample.outputBytes = outputBytes;
                sample.outputQueueLatency = queueLatency;
                callback(sample);
            }
        });
    }

    if (metricCallback) {
        LogicLoopMetricSample sample;
        sample.event = LogicLoopMetricEvent::OutputDispatched;
        sample.loop = logicLoop_.load(std::memory_order_acquire);
        sample.outputBatch = outputBatch;
        sample.queuedOutputs = queuedOutputs;
        sample.droppedOutputs = droppedOutputs;
        sample.outputBytes = outputBytes;
        metricCallback(sample);
    }
}

}  // namespace mini::game::logic
