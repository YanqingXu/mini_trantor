#include "mini/game/SessionManager.h"

#include <cassert>

namespace mini::game {

PlayerSessionPtr SessionManager::ensureSession(SessionToken sessionToken,
                                             mini::net::transport::TransportSessionId transportSessionId,
                                             bool autoStartAuth) {
    if (sessionToken.empty()) {
        return nullptr;
    }

    PlayerSessionPtr session;
    bool shouldStartAuth = false;
    bool shouldReconnect = false;
    PlayerSession::State oldState = PlayerSession::State::kCreated;

    {
        std::scoped_lock lock(mutex_);
        auto it = sessions_.find(sessionToken);
        if (it == sessions_.end()) {
            session = std::make_shared<PlayerSession>(
                std::move(sessionToken),
                transportSessionId);
            sessions_.emplace(session->sessionId(), session);

            if (transportSessionId != mini::net::transport::kInvalidTransportSessionId) {
                transportIndex_[transportSessionId] = session->sessionId();
            }
            oldState = PlayerSession::State::kCreated;
            shouldStartAuth = autoStartAuth;
        } else {
            session = it->second;
            oldState = session->state();
            if (autoStartAuth && oldState == PlayerSession::State::kCreated) {
                shouldStartAuth = true;
            }
            if (transportSessionId != mini::net::transport::kInvalidTransportSessionId) {
                const auto oldTransport = session->transportSessionId();
                const bool rebound = setTransportIndexLocked(
                    session->sessionId(), oldTransport, transportSessionId, session);
                if (rebound && oldState == PlayerSession::State::kClosing) {
                    shouldReconnect = true;
                }
                if (!rebound) {
                    shouldStartAuth = false;
                }
            }
        }
    }

    if (shouldStartAuth && session) {
        const auto old = oldState;
        const bool started = session->startAuthentication();
        if (started) {
            const auto now = session->state();
            emitState(session->sessionId(), old, now, "auth start");
        }
    }

    if (shouldReconnect && session) {
        markReconnecting(session->sessionId());
    }

    return session;
}

PlayerSessionPtr SessionManager::getSession(std::string_view sessionToken) const {
    std::scoped_lock lock(mutex_);
    auto it = sessions_.find(std::string(sessionToken));
    if (it == sessions_.end()) {
        return nullptr;
    }
    return it->second;
}

std::weak_ptr<PlayerSession> SessionManager::getSessionWeak(std::string_view sessionToken) const {
    std::scoped_lock lock(mutex_);
    auto it = sessions_.find(std::string(sessionToken));
    if (it == sessions_.end()) {
        return {};
    }
    return it->second;
}

PlayerSessionPtr SessionManager::getSession(mini::net::transport::TransportSessionId transportSessionId) const {
    std::scoped_lock lock(mutex_);
    const auto it = transportIndex_.find(transportSessionId);
    if (it == transportIndex_.end()) {
        return nullptr;
    }

    auto sessionIt = sessions_.find(it->second);
    if (sessionIt == sessions_.end()) {
        return nullptr;
    }

    return sessionIt->second;
}

bool SessionManager::hasSession(std::string_view sessionToken) const {
    std::scoped_lock lock(mutex_);
    return sessions_.find(std::string(sessionToken)) != sessions_.end();
}

std::size_t SessionManager::sessionCount() const {
    std::scoped_lock lock(mutex_);
    return sessions_.size();
}

bool SessionManager::markStartAuth(std::string_view sessionToken) {
    auto session = getSession(sessionToken);
    if (!session) {
        return false;
    }
    const auto old = session->state();
    const bool changed = session->startAuthentication();
    if (changed) {
        emitState(std::string(sessionToken), old, session->state(), "auth start");
    }
    return changed;
}

bool SessionManager::authenticate(std::string_view sessionToken,
                                 std::uint64_t playerId,
                                 std::string_view playerName,
                                 std::string_view role) {
    auto session = getSession(sessionToken);
    if (!session) {
        return false;
    }
    const auto old = session->state();
    const bool changed = session->markAuthenticated(playerId, playerName, role);
    if (changed) {
        emitState(std::string(sessionToken), old, session->state(), "auth success");
    }
    return changed;
}

bool SessionManager::markOnline(std::string_view sessionToken) {
    auto session = getSession(sessionToken);
    if (!session) {
        return false;
    }
    const auto old = session->state();
    const bool changed = session->markOnline();
    if (changed) {
        emitState(std::string(sessionToken), old, session->state(), "online");
    }
    return changed;
}

bool SessionManager::refreshHeartbeat(std::string_view sessionToken) {
    auto session = getSession(sessionToken);
    if (!session) {
        return false;
    }
    const auto old = session->state();
    const bool changed = session->refreshHeartbeat();
    const auto next = session->state();
    if (changed && old != next) {
        emitState(std::string(sessionToken), old, next, "heartbeat");
    }
    return changed;
}

bool SessionManager::onAuthTimeout(std::string_view sessionToken) {
    auto session = getSession(sessionToken);
    if (!session) {
        return false;
    }
    const auto old = session->state();
    const bool changed = session->onAuthTimeout();
    const auto next = session->state();
    if (changed && old != next) {
        emitState(std::string(sessionToken), old, next, "auth timeout");
    }
    return changed;
}

bool SessionManager::onHeartbeatTimeout(std::string_view sessionToken) {
    auto session = getSession(sessionToken);
    if (!session) {
        return false;
    }
    const auto old = session->state();
    const bool changed = session->onHeartbeatTimeout();
    const auto next = session->state();
    if (changed && old != next) {
        emitState(std::string(sessionToken), old, next, "heartbeat timeout");
    }
    return changed;
}

bool SessionManager::onConnectionClose(mini::net::transport::TransportSessionId transportSessionId,
                                      std::string_view reason) {
    auto session = getSession(transportSessionId);
    if (!session) {
        return false;
    }

    const auto token = session->sessionId();
    const auto old = session->state();
    const bool changed = session->onConnectionClose(reason);

    if (changed) {
        emitState(token, old, session->state(), reason);
    }

    {
        std::scoped_lock lock(mutex_);
        const auto mapIt = transportIndex_.find(transportSessionId);
        if (mapIt != transportIndex_.end() && mapIt->second == token) {
            transportIndex_.erase(mapIt);
        }
        endpointIndex_.erase(transportSessionId);
    }

    session->detachTransport();
    if (changed) {
        scheduleReconnectWindow(token);
    }
    return changed;
}

bool SessionManager::markReconnecting(std::string_view sessionToken) {
    auto session = getSession(sessionToken);
    if (!session) {
        return false;
    }

    const auto old = session->state();
    SessionMetricSample metric;
    metric.event = SessionMetricEvent::ReconnectSucceeded;
    metric.sessionToken = std::string(sessionToken);
    metric.success = true;
    {
        std::scoped_lock lock(mutex_);
        const auto startedIt = reconnectStartedAt_.find(std::string(sessionToken));
        if (startedIt != reconnectStartedAt_.end()) {
            metric.reconnectDuration = mini::base::now() - startedIt->second;
            reconnectStartedAt_.erase(startedIt);
        }
    }
    cancelReconnectWindow(std::string(sessionToken));
    const bool changed = session->markReconnecting();
    if (changed) {
        emitState(std::string(sessionToken), old, session->state(), "reconnecting");
        emitSessionMetric(std::move(metric));
    }
    return changed;
}

bool SessionManager::markClosed(std::string_view sessionToken, std::string_view reason) {
    auto session = getSession(sessionToken);
    if (!session) {
        return false;
    }
    cancelReconnectWindow(std::string(sessionToken));
    const auto old = session->state();
    const bool changed = session->close(reason);
    if (changed) {
        emitState(std::string(sessionToken), old, session->state(), reason);
    }
    return changed;
}

bool SessionManager::bindTransport(std::string_view sessionToken,
                                  mini::net::transport::TransportSessionId transportSessionId) {
    PlayerSessionPtr session;
    mini::net::transport::TransportSessionId oldTransport = mini::net::transport::kInvalidTransportSessionId;
    State oldState = State::kClosed;
    bool changed = false;

    {
        std::scoped_lock lock(mutex_);
        const auto it = sessions_.find(std::string(sessionToken));
        if (it == sessions_.end()) {
            return false;
        }

        session = it->second;
        if (!session) {
            return false;
        }

        oldTransport = session->transportSessionId();
        oldState = session->state();
        changed = setTransportIndexLocked(session->sessionId(), oldTransport, transportSessionId, session);
    }

    if (!changed || !session) {
        return false;
    }

    if (oldState == PlayerSession::State::kClosing) {
        if (markReconnecting(sessionToken)) {
            cancelReconnectWindow(std::string(sessionToken));
        }
    }

    return true;
}

bool SessionManager::bindTransportEndpoint(
    std::string_view sessionToken,
    const std::shared_ptr<mini::net::transport::ITransportEndpoint>& endpoint) {
    if (!endpoint) {
        return false;
    }

    const auto transportSessionId = endpoint->sessionId();
    if (transportSessionId == mini::net::transport::kInvalidTransportSessionId) {
        return false;
    }

    const bool rebound = bindTransport(sessionToken, transportSessionId);
    if (!rebound) {
        auto existing = getSession(sessionToken);
        if (!existing || existing->transportSessionId() != transportSessionId) {
            return false;
        }
    }

    {
        std::scoped_lock lock(mutex_);
        endpointIndex_[transportSessionId] = endpoint;
    }
    return true;
}

bool SessionManager::onReconnect(std::string_view sessionToken,
                                mini::net::transport::TransportSessionId transportSessionId) {
    if (!bindTransport(sessionToken, transportSessionId)) {
        return false;
    }
    return markReconnecting(sessionToken);
}

bool SessionManager::removeSession(std::string_view sessionToken) {
    cancelReconnectWindow(std::string(sessionToken));
    PlayerSessionPtr session;
    {
        std::scoped_lock lock(mutex_);
        auto it = sessions_.find(std::string(sessionToken));
        if (it == sessions_.end()) {
            return false;
        }

        session = it->second;
        sessions_.erase(it);

        const auto transport = session->transportSessionId();
        if (transport != mini::net::transport::kInvalidTransportSessionId) {
            transportIndex_.erase(transport);
            endpointIndex_.erase(transport);
        }
        reconnectEpoch_.erase(std::string(sessionToken));
        reconnectTimer_.erase(std::string(sessionToken));
        reconnectStartedAt_.erase(std::string(sessionToken));
    }

    const auto token = session->sessionId();
    const auto old = session->state();
    const bool changed = session->close("removed by manager");
    if (changed) {
        emitState(token, old, session->state(), "removed by manager");
    }
    return true;
}

bool SessionManager::removeSessionByTransport(mini::net::transport::TransportSessionId transportSessionId) {
    std::string token;
    {
        std::scoped_lock lock(mutex_);
        const auto it = transportIndex_.find(transportSessionId);
        if (it == transportIndex_.end()) {
            return false;
        }
        token = it->second;
    }
    if (token.empty()) {
        return false;
    }
    return removeSession(token);
}

bool SessionManager::cleanupClosedSessions(std::chrono::steady_clock::time_point now) {
    std::vector<std::string> closed;

    {
        std::scoped_lock lock(mutex_);
        closed.reserve(sessions_.size());
        for (const auto& [token, session] : sessions_) {
            if (!session) {
                continue;
            }

            if (session->isClosed()) {
                closed.push_back(token);
                continue;
            }

            if (session->onAuthTimeout(now)) {
                closed.push_back(token);
            }
        }
    }

    bool any = false;
    for (const auto& token : closed) {
        any = removeSession(token) || any;
    }
    return any;
}

void SessionManager::emitState(const SessionToken& sessionToken,
                              State oldState,
                              State newState,
                              std::string_view reason) {
    if (oldState == newState) {
        return;
    }

    SessionStateCallback callback;
    {
        std::scoped_lock lock(mutex_);
        callback = stateCallback_;
    }
    if (!callback) {
        return;
    }

    auto safeReason = std::string(reason);
    if (!logicLoop_ || logicLoop_->isInLoopThread()) {
        callback(sessionToken, oldState, newState, safeReason);
        return;
    }

    postOnLogicLoop([callback = std::move(callback),
                    sessionToken,
                    oldState,
                    newState,
                    reason = std::move(safeReason)]() mutable {
        callback(sessionToken, oldState, newState, reason);
    });
}

void SessionManager::emitSessionMetric(SessionMetricSample sample) {
    SessionMetricCallback callback;
    {
        std::scoped_lock lock(mutex_);
        callback = metricCallback_;
    }
    if (!callback) {
        return;
    }

    if (!logicLoop_ || logicLoop_->isInLoopThread()) {
        callback(sample);
        return;
    }

    postOnLogicLoop([callback = std::move(callback), sample = std::move(sample)]() mutable {
        callback(sample);
    });
}

void SessionManager::setReconnectWindow(PlayerSession::Milliseconds reconnectWindow) {
    if (reconnectWindow <= PlayerSession::Milliseconds::zero()) {
        reconnectWindow = kDefaultReconnectWindow;
    }
    std::scoped_lock lock(mutex_);
    reconnectWindow_ = reconnectWindow;
}

PlayerSession::Milliseconds SessionManager::reconnectWindow() const {
    std::scoped_lock lock(mutex_);
    return reconnectWindow_;
}

std::shared_ptr<mini::net::transport::ITransportEndpoint>
SessionManager::getTransportEndpoint(std::string_view sessionToken) const {
    auto session = getSession(sessionToken);
    if (!session) {
        return nullptr;
    }
    return getTransportEndpoint(session->transportSessionId());
}

std::shared_ptr<mini::net::transport::ITransportEndpoint>
SessionManager::getTransportEndpoint(mini::net::transport::TransportSessionId transportSessionId) const {
    std::scoped_lock lock(mutex_);
    auto it = endpointIndex_.find(transportSessionId);
    if (it == endpointIndex_.end()) {
        return nullptr;
    }
    return it->second.lock();
}

void SessionManager::scheduleReconnectWindow(const SessionToken& sessionToken) {
    const auto token = sessionToken;
    postOnLogicLoop([this, token] {
        if (!logicLoop_) {
            return;
        }

        {
            std::scoped_lock lock(mutex_);
            if (reconnectWindow_ <= PlayerSession::Milliseconds::zero()) {
                return;
            }

            auto& epoch = reconnectEpoch_[token];
            ++epoch;
            const auto currentEpoch = epoch;

            const auto it = reconnectTimer_.find(token);
            if (it != reconnectTimer_.end() && it->second.valid()) {
                logicLoop_->cancel(it->second);
            }

            const auto timerId = logicLoop_->runAfter(reconnectWindow_, [this, token, currentEpoch] {
                onReconnectWindowExpired(token, currentEpoch);
            });
            reconnectTimer_[token] = timerId;
            reconnectStartedAt_[token] = mini::base::now();
        }

        SessionMetricSample sample;
        sample.event = SessionMetricEvent::ReconnectWindowStarted;
        sample.sessionToken = token;
        emitSessionMetric(std::move(sample));
    });
}

void SessionManager::cancelReconnectWindow(const SessionToken& sessionToken) {
    postOnLogicLoop([this, token = sessionToken] {
        std::scoped_lock lock(mutex_);
        cancelReconnectWindowLocked(token);
    });
}

void SessionManager::cancelReconnectWindowLocked(const SessionToken& sessionToken) {
    const auto it = reconnectTimer_.find(sessionToken);
    if (it == reconnectTimer_.end()) {
        reconnectEpoch_.erase(sessionToken);
        return;
    }

    if (logicLoop_ && it->second.valid()) {
        logicLoop_->cancel(it->second);
    }

    reconnectTimer_.erase(it);
    reconnectEpoch_.erase(sessionToken);
    reconnectStartedAt_.erase(sessionToken);
}

void SessionManager::onReconnectWindowExpired(const SessionToken& sessionToken, std::uint64_t epoch) {
    if (!logicLoop_) {
        return;
    }

    PlayerSessionPtr session;

    std::unique_lock lock(mutex_);
    const auto epochIt = reconnectEpoch_.find(sessionToken);
    if (epochIt == reconnectEpoch_.end() || epochIt->second != epoch) {
        return;
    }

    auto sessionIt = sessions_.find(sessionToken);
    if (sessionIt == sessions_.end() || !sessionIt->second) {
        cancelReconnectWindowLocked(sessionToken);
        return;
    }

    session = sessionIt->second;
    if (!session || session->state() != PlayerSession::State::kClosing) {
        cancelReconnectWindowLocked(sessionToken);
        return;
    }

    const auto old = session->state();
    SessionMetricSample metric;
    metric.event = SessionMetricEvent::ReconnectExpired;
    metric.sessionToken = sessionToken;
    metric.success = false;
    const auto startedIt = reconnectStartedAt_.find(sessionToken);
    if (startedIt != reconnectStartedAt_.end()) {
        metric.reconnectDuration = mini::base::now() - startedIt->second;
        reconnectStartedAt_.erase(startedIt);
    }
    lock.unlock();

    const bool changed = session->close("reconnect timeout");
    if (changed) {
        emitState(sessionToken, old, session->state(), "reconnect timeout");
        emitSessionMetric(std::move(metric));
    }
    cancelReconnectWindow(sessionToken);
    removeSession(sessionToken);
}

bool SessionManager::setTransportIndexLocked(const SessionToken& sessionToken,
                                           mini::net::transport::TransportSessionId oldTransportId,
                                           mini::net::transport::TransportSessionId newTransportId,
                                           const PlayerSessionPtr& session) {
    if (newTransportId == mini::net::transport::kInvalidTransportSessionId) {
        if (oldTransportId != mini::net::transport::kInvalidTransportSessionId) {
            transportIndex_.erase(oldTransportId);
            endpointIndex_.erase(oldTransportId);
        }
        return session->detachTransport();
    }

    if (oldTransportId == newTransportId) {
        return false;
    }

    if (newTransportId != mini::net::transport::kInvalidTransportSessionId) {
        const auto existing = transportIndex_.find(newTransportId);
        if (existing != transportIndex_.end()) {
            if (existing->second != sessionToken) {
                const auto& oldOwnerToken = existing->second;
                const auto oldOwnerIt = sessions_.find(oldOwnerToken);
                if (oldOwnerIt != sessions_.end() && oldOwnerIt->second) {
                    oldOwnerIt->second->detachTransport();
                }
            }
            transportIndex_.erase(existing);
            endpointIndex_.erase(newTransportId);
        }
    }

    if (oldTransportId != mini::net::transport::kInvalidTransportSessionId) {
        transportIndex_.erase(oldTransportId);
        endpointIndex_.erase(oldTransportId);
    }

    session->bindTransportSession(newTransportId);
    transportIndex_[newTransportId] = sessionToken;
    return true;
}

template <typename Fn>
void SessionManager::postOnLogicLoop(Fn&& fn) const {
    if (!logicLoop_) {
        return;
    }

    if (logicLoop_->isInLoopThread()) {
        fn();
        return;
    }

    logicLoop_->queueInLoop(std::forward<Fn>(fn));
}

}  // namespace mini::game
