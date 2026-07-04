#include "mini/game/GameServerPipeline.h"

#include "mini/game/SessionManager.h"
#include "mini/game/logic/LogicLoop.h"
#include "mini/base/Timestamp.h"
#include "mini/net/Buffer.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"
#include "mini/net/transport/TransportEndpoint.h"
#include "mini/net/transport/TransportManager.h"

#include <any>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace mini::game {

GameServerPipeline::GameServerPipeline(
    net::TcpServer& server,
    net::transport::TransportManager& transportManager,
    SessionManager& sessionManager,
    logic::LogicLoop& logicLoop)
    : GameServerPipeline(server, transportManager, sessionManager, logicLoop, Options{}) {
}

GameServerPipeline::GameServerPipeline(
    net::TcpServer& server,
    net::transport::TransportManager& transportManager,
    SessionManager& sessionManager,
    logic::LogicLoop& logicLoop,
    Options options)
    : server_(server),
      transportManager_(transportManager),
      sessionManager_(sessionManager),
      logicLoop_(logicLoop),
      options_(std::move(options)) {
    options_.validate();
}

GameServerPipeline::~GameServerPipeline() {
    lifetimeToken_.reset();
}

void GameServerPipeline::install() {
    std::weak_ptr<void> lifetime = lifetimeToken_;
    logicLoop_.setOutputBackpressurePolicy(options_.backpressure.output);
    logicLoop_.setBackpressureMetricCallback([this, lifetime](const GameBackpressureMetricSample& sample) {
        if (!lifetime.lock()) {
            return;
        }
        emitBackpressureMetric(sample);
    });
    server_.setConnectionCallback([this, lifetime](const std::shared_ptr<net::TcpConnection>& connection) {
        if (!lifetime.lock()) {
            return;
        }
        onConnection(connection);
    });
    server_.setMessageCallback([this, lifetime](const std::shared_ptr<net::TcpConnection>& connection, net::Buffer* buffer) {
        if (!lifetime.lock()) {
            return;
        }
        onMessage(connection, buffer);
    });
    if (options_.backpressure.broadcast.hardFanoutConnections > 0 ||
        options_.backpressure.broadcast.hardPayloadBytes > 0) {
        server_.setBroadcastAdmissionCallback(
            [this, lifetime](const net::BroadcastMetricSample& sample) {
                if (!lifetime.lock()) {
                    return true;
                }
                return admitBroadcast(sample);
            });
    }
}

const GameServerPipeline::Options& GameServerPipeline::options() const noexcept {
    return options_;
}

void GameServerPipeline::setMetricCallback(GamePipelineMetricCallback callback) {
    std::scoped_lock lock(metricMutex_);
    metricCallback_ = std::move(callback);
}

void GameServerPipeline::setBackpressureMetricCallback(GameBackpressureMetricCallback callback) {
    std::scoped_lock lock(metricMutex_);
    backpressureMetricCallback_ = std::move(callback);
}

void GameServerPipeline::setSecurityMetricCallback(GameSecurityMetricCallback callback) {
    std::scoped_lock lock(metricMutex_);
    securityMetricCallback_ = std::move(callback);
}

void GameServerPipeline::setAuthTokenValidator(AuthTokenValidator validator) {
    std::scoped_lock lock(securityMutex_);
    authTokenValidator_ = std::move(validator);
}

void GameServerPipeline::onConnection(const std::shared_ptr<net::TcpConnection>& connection) {
    if (!connection) {
        return;
    }

    if (!connection->connected()) {
        auto* statePtr = std::any_cast<std::shared_ptr<ConnectionState>>(&connection->getContext());
        if (!statePtr || !*statePtr) {
            return;
        }

        const auto& state = *statePtr;
        if (!state->sessionToken.empty()) {
            server_.unbindBroadcastSession(connection, state->sessionToken);
            sessionManager_.postConnectionClose(state->transportSessionId, "network close");
        }
        transportManager_.deregisterEndpoint(state->transportSessionId);
        return;
    }

    auto endpoint = net::transport::TransportEndpoint::create(connection);
    const auto transportSessionId = transportManager_.registerEndpoint(endpoint);
    auto state = std::make_shared<ConnectionState>();
    state->transportSessionId = transportSessionId;
    state->endpoint = endpoint;
    connection->setContext(state);
}

void GameServerPipeline::onMessage(const std::shared_ptr<net::TcpConnection>& connection, net::Buffer* buffer) {
    if (!connection || !buffer) {
        return;
    }

    auto* statePtr = std::any_cast<std::shared_ptr<ConnectionState>>(&connection->getContext());
    if (!statePtr || !*statePtr) {
        closeForSecurity(connection,
                         nullptr,
                         GameSecurityMetricEvent::AbnormalClose,
                         GameSecurityReason::PipelineStateMissing,
                         0,
                         buffer->readableBytes());
        return;
    }
    auto state = *statePtr;
    state->input += buffer->retrieveAllAsString();
    if (rejectInputIfOverHardLimit(connection, state)) {
        return;
    }
    processInput(connection, state);
}

void GameServerPipeline::processInput(
    const std::shared_ptr<net::TcpConnection>& connection,
    const std::shared_ptr<ConnectionState>& state) {
    if (!connection || !state) {
        return;
    }
    if (state->processingInput) {
        return;
    }
    state->processingInput = true;

    const auto batchStartedAt = mini::base::now();
    std::size_t offset = 0;
    std::size_t frames = 0;
    while (offset < state->input.size() && frames < net::framing::kDefaultMaxFramesPerBatch) {
        net::framing::Packet packet;
        std::size_t consumed = 0;
        const auto decodeState = state->framer.decode(
            state->input.data() + offset,
            state->input.size() - offset,
            packet,
            consumed);
        if (decodeState == net::framing::PacketDecodeState::kNeedMore) {
            break;
        }
        if (decodeState != net::framing::PacketDecodeState::kComplete) {
            state->processingInput = false;
            closeForSecurity(connection,
                             state,
                             GameSecurityMetricEvent::AbnormalClose,
                             GameSecurityReason::InvalidFrame,
                             0,
                             state->input.size(),
                             state->input.size());
            return;
        }

        handleFrame(connection, state, packet);
        offset += consumed;
        ++frames;
        if (!connection->connected()) {
            break;
        }
    }

    if (offset > 0) {
        state->input.erase(0, offset);
    }
    state->processingInput = false;

    const bool shouldContinue = frames >= net::framing::kDefaultMaxFramesPerBatch && !state->input.empty();
    if (shouldContinue) {
        scheduleInputContinuation(connection, state);
        GameBackpressureMetricSample bpSample;
        bpSample.event = GameBackpressureMetricEvent::InputDeferred;
        bpSample.layer = GameBackpressureLayer::InputFraming;
        bpSample.action = GameBackpressureAction::Defer;
        bpSample.reason = GameBackpressureReason::FrameBatchBudget;
        bpSample.loop = connection->getLoop();
        bpSample.sessionToken = state->sessionToken;
        bpSample.transportSessionId = state->transportSessionId;
        bpSample.currentValue = state->input.size();
        bpSample.backlog = state->input.size();
        emitBackpressureMetric(std::move(bpSample));
    }

    if (frames > 0 || offset > 0 || shouldContinue) {
        GamePipelineMetricSample sample;
        sample.event = GamePipelineMetricEvent::InputBatchProcessed;
        sample.loop = connection->getLoop();
        sample.sessionToken = state->sessionToken;
        sample.framesDecoded = frames;
        sample.bytesConsumed = offset;
        sample.bufferedBytes = state->input.size();
        sample.continuationScheduled = shouldContinue;
        sample.batchDuration = mini::base::now() - batchStartedAt;
        emitMetric(std::move(sample));
    }
}

void GameServerPipeline::scheduleInputContinuation(
    const std::shared_ptr<net::TcpConnection>& connection,
    const std::shared_ptr<ConnectionState>& state) {
    if (!connection || !state || state->continuationScheduled) {
        return;
    }
    auto* loop = connection->getLoop();
    if (!loop) {
        return;
    }

    state->continuationScheduled = true;
    std::weak_ptr<void> lifetime = lifetimeToken_;
    std::weak_ptr<net::TcpConnection> weakConnection = connection;
    loop->queueInLoop([this, lifetime, weakConnection, state] {
        state->continuationScheduled = false;
        if (!lifetime.lock()) {
            return;
        }
        auto connection = weakConnection.lock();
        if (!connection || !connection->connected()) {
            return;
        }
        processInput(connection, state);
    });
}

void GameServerPipeline::handleFrame(
    const std::shared_ptr<net::TcpConnection>& connection,
    const std::shared_ptr<ConnectionState>& state,
    const net::framing::Packet& packet) {
    if (!connection || !state || !state->endpoint) {
        return;
    }

    const auto payload = std::string(packet.payload);
    if (packet.header.msgId == options_.authMsgId) {
        AuthFrame authFrame;
        if (rejectAuthIfDenied(connection, state, packet, payload, authFrame)) {
            return;
        }

        state->sessionToken = authFrame.sessionToken;
        state->authNonce = authFrame.nonce;
        auto session = sessionManager_.ensureSession(authFrame.sessionToken, state->transportSessionId, false);
        if (!session) {
            closeForSecurity(connection,
                             state,
                             GameSecurityMetricEvent::AuthRejected,
                             GameSecurityReason::SessionEnsureFailed,
                             packet.header.msgId,
                             payload.size());
            return;
        }

        sessionManager_.bindTransportEndpoint(authFrame.sessionToken, state->endpoint);
        if (session->state() == PlayerSession::State::kCreated) {
            sessionManager_.markStartAuth(authFrame.sessionToken);
        }
        if (session->state() == PlayerSession::State::kAuthenticating) {
            sessionManager_.authenticate(
                authFrame.sessionToken,
                state->transportSessionId,
                authFrame.sessionToken,
                "default");
        }
        sessionManager_.markOnline(authFrame.sessionToken);

        server_.bindBroadcastSession(connection, authFrame.sessionToken);
        server_.joinBroadcastGroup(authFrame.sessionToken, options_.defaultGroup);
        server_.joinBroadcastAoi(authFrame.sessionToken, options_.defaultAoi);

        state->authenticated = true;
        GameSecurityMetricSample sample;
        sample.event = GameSecurityMetricEvent::AuthAccepted;
        sample.loop = connection->getLoop();
        sample.sessionToken = state->sessionToken;
        sample.transportSessionId = state->transportSessionId;
        sample.msgId = packet.header.msgId;
        sample.payloadBytes = payload.size();
        emitSecurityMetric(std::move(sample));
        state->endpoint->send(state->framer.encode(
            options_.responseMsgId,
            0,
            packet.header.seq,
            "auth-ok"));
        return;
    }

    if (!state->authenticated) {
        closeForSecurity(connection,
                         state,
                         GameSecurityMetricEvent::AbnormalClose,
                         GameSecurityReason::UnauthenticatedFrame,
                         packet.header.msgId,
                         payload.size());
        return;
    }

    if (rejectIfSessionRateLimited(connection, state, packet, payload.size())) {
        return;
    }

    if (packet.header.msgId == options_.commandMsgId) {
        const auto priority = toMetricPriority(priorityFromPacketFlags(packet.header.flags));
        const bool submitted = logicLoop_.submit(
            state->sessionToken,
            state->transportSessionId,
            state->endpoint,
            payload,
            priority);
        GamePipelineMetricSample sample;
        sample.event = GamePipelineMetricEvent::LogicSubmitResult;
        sample.loop = connection->getLoop();
        sample.sessionToken = state->sessionToken;
        sample.msgId = packet.header.msgId;
        sample.logicSubmitted = submitted;
        sample.logicBacklog = logicLoop_.backlog();
        emitMetric(std::move(sample));
        return;
    }

    if (packet.header.msgId == options_.broadcastMsgId) {
        const auto priority = toMetricPriority(priorityFromPacketFlags(packet.header.flags));
        server_.broadcastGroup(
            options_.defaultGroup,
            state->framer.encode(options_.responseMsgId, 0, packet.header.seq, "broadcast:" + payload),
            priority);
        return;
    }
}

GameServerPipeline::AuthFrame GameServerPipeline::parseAuthFrame(
    std::string_view payload,
    bool splitOnDelimiter) const {
    AuthFrame frame;
    if (!splitOnDelimiter || options_.security.authTokenNonceDelimiter.empty()) {
        frame.sessionToken = std::string(payload);
        frame.nonce = std::string(payload);
        return frame;
    }

    const auto& delimiter = options_.security.authTokenNonceDelimiter;
    const auto pos = payload.find(delimiter);
    if (pos == std::string_view::npos) {
        frame.sessionToken = std::string(payload);
        frame.nonce = std::string(payload);
        return frame;
    }

    frame.sessionToken = std::string(payload.substr(0, pos));
    frame.nonce = std::string(payload.substr(pos + delimiter.size()));
    return frame;
}

bool GameServerPipeline::rejectAuthIfDenied(
    const std::shared_ptr<net::TcpConnection>& connection,
    const std::shared_ptr<ConnectionState>& state,
    const net::framing::Packet& packet,
    std::string_view payload,
    AuthFrame& authFrame) {
    AuthTokenValidator validator;
    {
        std::scoped_lock lock(securityMutex_);
        validator = authTokenValidator_;
    }

    authFrame = parseAuthFrame(
        payload,
        options_.security.replayProtectionEnabled() || static_cast<bool>(validator));

    if (payload.empty() || authFrame.sessionToken.empty()) {
        closeForSecurity(connection,
                         state,
                         GameSecurityMetricEvent::AuthRejected,
                         GameSecurityReason::EmptyAuthToken,
                         packet.header.msgId,
                         payload.size());
        return true;
    }

    if (options_.security.maxAuthTokenBytes > 0 &&
        payload.size() > options_.security.maxAuthTokenBytes) {
        closeForSecurity(connection,
                         state,
                         GameSecurityMetricEvent::AuthRejected,
                         GameSecurityReason::AuthTokenTooLarge,
                         packet.header.msgId,
                         payload.size(),
                         payload.size(),
                         options_.security.maxAuthTokenBytes,
                         authFrame.sessionToken);
        return true;
    }

    if (options_.security.replayProtectionEnabled() && authFrame.nonce.empty()) {
        closeForSecurity(connection,
                         state,
                         GameSecurityMetricEvent::AuthRejected,
                         GameSecurityReason::EmptyAuthNonce,
                         packet.header.msgId,
                         payload.size(),
                         0,
                         0,
                         authFrame.sessionToken);
        return true;
    }

    if (validator && !validator(authFrame.sessionToken, authFrame.nonce)) {
        closeForSecurity(connection,
                         state,
                         GameSecurityMetricEvent::AuthRejected,
                         GameSecurityReason::AuthTokenValidatorRejected,
                         packet.header.msgId,
                         payload.size(),
                         0,
                         0,
                         authFrame.sessionToken);
        return true;
    }

    if (!options_.security.replayProtectionEnabled()) {
        return false;
    }

    const auto now = mini::base::now();
    bool replayed = false;
    {
        std::scoped_lock lock(securityMutex_);
        pruneExpiredAuthReplayLocked(now);
        const auto key = authReplayKey(authFrame.sessionToken, authFrame.nonce);
        const auto it = authReplayExpirations_.find(key);
        if (it != authReplayExpirations_.end() && it->second > now) {
            replayed = true;
        } else {
            authReplayExpirations_[key] = now + options_.security.authReplayWindow;
        }
    }

    if (replayed) {
        closeForSecurity(connection,
                         state,
                         GameSecurityMetricEvent::AuthRejected,
                         GameSecurityReason::AuthReplay,
                         packet.header.msgId,
                         payload.size(),
                         0,
                         0,
                         authFrame.sessionToken);
        return true;
    }

    return false;
}

bool GameServerPipeline::rejectIfSessionRateLimited(
    const std::shared_ptr<net::TcpConnection>& connection,
    const std::shared_ptr<ConnectionState>& state,
    const net::framing::Packet& packet,
    std::size_t payloadBytes) {
    if (!options_.security.sessionRateLimitEnabled()) {
        return false;
    }
    if (!connection || !state || state->sessionToken.empty()) {
        return false;
    }

    const auto now = mini::base::now();
    std::size_t currentValue = 0;
    const auto limit = options_.security.maxFramesPerSessionPerWindow;
    bool limited = false;
    {
        std::scoped_lock lock(securityMutex_);
        pruneExpiredRateStatesLocked(now);
        auto& rate = sessionRate_[state->sessionToken];
        if (rate.framesInWindow == 0 ||
            now - rate.windowStartedAt >= options_.security.sessionRateWindow) {
            rate.windowStartedAt = now;
            rate.framesInWindow = 0;
        }
        rate.lastSeenAt = now;
        ++rate.framesInWindow;
        currentValue = rate.framesInWindow;
        limited = currentValue > limit;
    }

    if (!limited) {
        return false;
    }

    closeForSecurity(connection,
                     state,
                     GameSecurityMetricEvent::RateLimited,
                     GameSecurityReason::SessionRateLimit,
                     packet.header.msgId,
                     payloadBytes,
                     currentValue,
                     limit);
    return true;
}

void GameServerPipeline::pruneExpiredAuthReplayLocked(mini::base::Timestamp now) {
    for (auto it = authReplayExpirations_.begin(); it != authReplayExpirations_.end();) {
        if (it->second <= now) {
            it = authReplayExpirations_.erase(it);
        } else {
            ++it;
        }
    }
}

void GameServerPipeline::pruneExpiredRateStatesLocked(mini::base::Timestamp now) {
    if (!options_.security.sessionRateLimitEnabled()) {
        return;
    }

    const auto ttl = options_.security.sessionRateWindow + options_.security.sessionRateWindow;
    for (auto it = sessionRate_.begin(); it != sessionRate_.end();) {
        const auto& rate = it->second;
        if (rate.lastSeenAt != mini::base::Timestamp{} && now - rate.lastSeenAt > ttl) {
            it = sessionRate_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string GameServerPipeline::authReplayKey(std::string_view sessionToken, std::string_view nonce) {
    std::string key(sessionToken);
    key.push_back('\0');
    key.append(nonce);
    return key;
}

void GameServerPipeline::emitMetric(GamePipelineMetricSample sample) {
    GamePipelineMetricCallback callback;
    {
        std::scoped_lock lock(metricMutex_);
        callback = metricCallback_;
    }
    if (!callback) {
        return;
    }
    callback(sample);
}

void GameServerPipeline::emitBackpressureMetric(GameBackpressureMetricSample sample) {
    GameBackpressureMetricCallback callback;
    {
        std::scoped_lock lock(metricMutex_);
        callback = backpressureMetricCallback_;
    }
    if (!callback) {
        return;
    }
    callback(sample);
}

void GameServerPipeline::emitSecurityMetric(GameSecurityMetricSample sample) {
    GameSecurityMetricCallback callback;
    {
        std::scoped_lock lock(metricMutex_);
        callback = securityMetricCallback_;
    }
    if (!callback) {
        return;
    }
    callback(sample);
}

void GameServerPipeline::closeForSecurity(
    const std::shared_ptr<net::TcpConnection>& connection,
    const std::shared_ptr<ConnectionState>& state,
    GameSecurityMetricEvent event,
    GameSecurityReason reason,
    std::uint32_t msgId,
    std::size_t payloadBytes,
    std::size_t currentValue,
    std::size_t limit,
    std::string_view sessionTokenOverride) {
    GameSecurityMetricSample sample;
    sample.event = event;
    sample.reason = reason;
    sample.loop = connection ? connection->getLoop() : nullptr;
    if (!sessionTokenOverride.empty()) {
        sample.sessionToken = std::string(sessionTokenOverride);
    } else if (state) {
        sample.sessionToken = state->sessionToken;
    }
    if (state) {
        sample.transportSessionId = state->transportSessionId;
    }
    sample.msgId = msgId;
    sample.payloadBytes = payloadBytes;
    sample.currentValue = currentValue;
    sample.limit = limit;
    emitSecurityMetric(sample);

    if (event != GameSecurityMetricEvent::AbnormalClose) {
        sample.event = GameSecurityMetricEvent::AbnormalClose;
        emitSecurityMetric(sample);
    }

    if (connection) {
        connection->forceClose();
    }
}

bool GameServerPipeline::rejectInputIfOverHardLimit(
    const std::shared_ptr<net::TcpConnection>& connection,
    const std::shared_ptr<ConnectionState>& state) {
    if (!connection || !state) {
        return false;
    }

    const auto hardLimit = options_.backpressure.input.hardBufferedBytes;
    if (hardLimit == 0 || state->input.size() <= hardLimit) {
        return false;
    }

    GameBackpressureMetricSample sample;
    sample.event = GameBackpressureMetricEvent::InputRejected;
    sample.layer = GameBackpressureLayer::InputFraming;
    sample.action = GameBackpressureAction::Close;
    sample.reason = GameBackpressureReason::InputBufferedBytesHardLimit;
    sample.loop = connection->getLoop();
    sample.sessionToken = state->sessionToken;
    sample.transportSessionId = state->transportSessionId;
    sample.currentValue = state->input.size();
    sample.softLimit = options_.backpressure.input.softBufferedBytes;
    sample.hardLimit = hardLimit;
    sample.backlog = state->input.size();
    emitBackpressureMetric(std::move(sample));

    connection->forceClose();
    return true;
}

bool GameServerPipeline::admitBroadcast(const net::BroadcastMetricSample& routed) {
    const auto& policy = options_.backpressure.broadcast;
    auto event = GameBackpressureMetricEvent::BroadcastAccepted;
    auto action = GameBackpressureAction::Accept;
    auto reason = GameBackpressureReason::None;
    std::size_t currentValue = 0;
    std::size_t softLimit = 0;
    std::size_t hardLimit = 0;
    bool accepted = true;
    const auto priority = routed.priority;

    if (policy.hardFanoutConnections > 0 &&
        routed.fanoutConnections > policy.hardFanoutConnections) {
        event = GameBackpressureMetricEvent::BroadcastRejected;
        action = GameBackpressureAction::Reject;
        reason = GameBackpressureReason::BroadcastFanoutHardLimit;
        currentValue = routed.fanoutConnections;
        softLimit = policy.softFanoutConnections;
        hardLimit = policy.hardFanoutConnections;
        accepted = false;
    } else if (policy.hardPayloadBytes > 0 &&
               routed.payloadBytes > policy.hardPayloadBytes) {
        event = GameBackpressureMetricEvent::BroadcastRejected;
        action = GameBackpressureAction::Reject;
        reason = GameBackpressureReason::BroadcastPayloadBytesHardLimit;
        currentValue = routed.payloadBytes;
        softLimit = policy.softPayloadBytes;
        hardLimit = policy.hardPayloadBytes;
        accepted = false;
    } else if (policy.priority.shouldDrop(
                   priority,
                   routed.fanoutConnections,
                   policy.softFanoutConnections,
                   policy.hardFanoutConnections)) {
        event = GameBackpressureMetricEvent::BroadcastRejected;
        action = GameBackpressureAction::DropLowPriority;
        reason = GameBackpressureReason::BroadcastFanoutSoftLimit;
        currentValue = routed.fanoutConnections;
        softLimit = policy.priority.effectiveSoftLimit(
            policy.softFanoutConnections,
            policy.hardFanoutConnections);
        hardLimit = policy.hardFanoutConnections;
        accepted = false;
    } else if (policy.priority.shouldDrop(
                   priority,
                   routed.payloadBytes,
                   policy.softPayloadBytes,
                   policy.hardPayloadBytes)) {
        event = GameBackpressureMetricEvent::BroadcastRejected;
        action = GameBackpressureAction::DropLowPriority;
        reason = GameBackpressureReason::BroadcastPayloadBytesSoftLimit;
        currentValue = routed.payloadBytes;
        softLimit = policy.priority.effectiveSoftLimit(
            policy.softPayloadBytes,
            policy.hardPayloadBytes);
        hardLimit = policy.hardPayloadBytes;
        accepted = false;
    } else if (policy.priority.effectiveSoftLimit(
                   policy.softFanoutConnections,
                   policy.hardFanoutConnections) > 0 &&
               routed.fanoutConnections >= policy.priority.effectiveSoftLimit(
                   policy.softFanoutConnections,
                   policy.hardFanoutConnections)) {
        reason = GameBackpressureReason::BroadcastFanoutSoftLimit;
        currentValue = routed.fanoutConnections;
        softLimit = policy.priority.effectiveSoftLimit(
            policy.softFanoutConnections,
            policy.hardFanoutConnections);
        hardLimit = policy.hardFanoutConnections;
    } else if (policy.priority.effectiveSoftLimit(
                   policy.softPayloadBytes,
                   policy.hardPayloadBytes) > 0 &&
               routed.payloadBytes >= policy.priority.effectiveSoftLimit(
                   policy.softPayloadBytes,
                   policy.hardPayloadBytes)) {
        reason = GameBackpressureReason::BroadcastPayloadBytesSoftLimit;
        currentValue = routed.payloadBytes;
        softLimit = policy.priority.effectiveSoftLimit(
            policy.softPayloadBytes,
            policy.hardPayloadBytes);
        hardLimit = policy.hardPayloadBytes;
    }

    GameBackpressureMetricSample sample;
    sample.event = event;
    sample.layer = GameBackpressureLayer::BroadcastFanout;
    sample.action = action;
    sample.reason = reason;
    sample.loop = routed.loop;
    sample.currentValue = currentValue;
    sample.softLimit = softLimit;
    sample.hardLimit = hardLimit;
    sample.priority = priority;
    sample.fanoutConnections = routed.fanoutConnections;
    sample.payloadBytes = routed.payloadBytes;
    sample.queueLatency = routed.routeLatency;
    emitBackpressureMetric(std::move(sample));

    return accepted;
}

}  // namespace mini::game
