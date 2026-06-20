#include "mini/game/PlayerSession.h"

#include <cassert>

namespace mini::game {

namespace {

bool shouldChangeState(PlayerSession::State cur, PlayerSession::State next) {
    return cur != next;
}

}  // namespace

PlayerSession::PlayerSession(SessionToken sessionToken,
                           mini::net::transport::TransportSessionId transportSessionId,
                           Milliseconds authTimeout,
                           Milliseconds heartbeatTimeout)
    : sessionToken_(std::move(sessionToken)),
      transportSessionId_(transportSessionId),
      createdAt_(mini::base::now()),
      lastActivityAt_(createdAt_),
      lastHeartbeatAt_(createdAt_),
      authTimeout_(authTimeout),
      heartbeatTimeout_(heartbeatTimeout),
      playerId_(0) {
    if (authTimeout_ <= Milliseconds::zero()) {
        authTimeout_ = kDefaultAuthTimeout;
    }
    if (heartbeatTimeout_ <= Milliseconds::zero()) {
        heartbeatTimeout_ = kDefaultHeartbeatTimeout;
    }
    authDeadline_ = createdAt_ + authTimeout_;
}

const PlayerSession::SessionToken& PlayerSession::sessionId() const noexcept {
    return sessionToken_;
}

mini::net::transport::TransportSessionId PlayerSession::transportSessionId() const noexcept {
    std::lock_guard lock(mutex_);
    return transportSessionId_;
}

PlayerSession::State PlayerSession::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

const char* PlayerSession::stateName(PlayerSession::State state) {
    switch (state) {
        case State::kCreated:
            return "Created";
        case State::kAuthenticating:
            return "Authenticating";
        case State::kAuthenticated:
            return "Authenticated";
        case State::kOnline:
            return "Online";
        case State::kHeartbeatTimeout:
            return "HeartbeatTimeout";
        case State::kReconnecting:
            return "Reconnecting";
        case State::kClosing:
            return "Closing";
        case State::kClosed:
            return "Closed";
    }
    return "Unknown";
}

const char* PlayerSession::stateName() const {
    return stateName(state());
}

bool PlayerSession::startAuthentication(TimePoint now) {
    std::lock_guard lock(mutex_);
    if (state_ != State::kCreated) {
        return false;
    }
    transition(State::kAuthenticating);
    authDeadline_ = now + authTimeout_;
    return true;
}

bool PlayerSession::markAuthenticated(std::uint64_t playerId,
                                    std::string_view playerName,
                                    std::string_view role) {
    std::lock_guard lock(mutex_);
    if (state_ != State::kAuthenticating && state_ != State::kCreated) {
        return false;
    }
    playerId_ = playerId;
    playerName_ = std::string(playerName);
    role_ = std::string(role);
    lastActivityAt_ = mini::base::now();
    lastHeartbeatAt_ = lastActivityAt_;
    transition(State::kAuthenticated);
    return true;
}

bool PlayerSession::markOnline(TimePoint now) {
    std::lock_guard lock(mutex_);
    if (state_ != State::kAuthenticated && state_ != State::kReconnecting &&
        state_ != State::kHeartbeatTimeout) {
        return false;
    }
    lastActivityAt_ = now;
    lastHeartbeatAt_ = now;
    transition(State::kOnline);
    return true;
}

bool PlayerSession::refreshHeartbeat(TimePoint now) {
    std::lock_guard lock(mutex_);
    if (state_ != State::kOnline && state_ != State::kHeartbeatTimeout) {
        return false;
    }
    if (state_ == State::kHeartbeatTimeout) {
        transition(State::kOnline);
    }
    lastActivityAt_ = now;
    lastHeartbeatAt_ = now;
    return true;
}

bool PlayerSession::onAuthTimeout(TimePoint now) {
    std::lock_guard lock(mutex_);
    if (state_ != State::kAuthenticating && state_ != State::kCreated) {
        return false;
    }
    if (now < authDeadline_) {
        return false;
    }
    closeReason_ = "auth timeout";
    transition(State::kClosed);
    return true;
}

bool PlayerSession::onHeartbeatTimeout(TimePoint now) {
    std::lock_guard lock(mutex_);
    if (state_ != State::kOnline) {
        return false;
    }
    if (now - lastHeartbeatAt_ < heartbeatTimeout_) {
        return false;
    }
    closeReason_ = "heartbeat timeout";
    transition(State::kHeartbeatTimeout);
    return true;
}

bool PlayerSession::onConnectionClose(std::string_view reason) {
    std::lock_guard lock(mutex_);
    if (state_ == State::kClosing || state_ == State::kClosed) {
        return false;
    }
    closeReason_ = std::string(reason);
    transition(State::kClosing);
    return true;
}

bool PlayerSession::markReconnecting() {
    std::lock_guard lock(mutex_);
    if (state_ != State::kClosing) {
        return false;
    }
    closeReason_.clear();
    transition(State::kReconnecting);
    return true;
}

bool PlayerSession::close(std::string_view reason) {
    std::lock_guard lock(mutex_);
    if (state_ == State::kClosed) {
        return false;
    }
    closeReason_ = std::string(reason);
    transition(State::kClosed);
    return true;
}

bool PlayerSession::bindTransportSession(mini::net::transport::TransportSessionId id) {
    std::lock_guard lock(mutex_);
    if (state_ == State::kClosed) {
        return false;
    }
    if (transportSessionId_ == id) {
        return false;
    }
    transportSessionId_ = id;
    return true;
}

bool PlayerSession::detachTransport() {
    std::lock_guard lock(mutex_);
    if (transportSessionId_ == mini::net::transport::kInvalidTransportSessionId) {
        return false;
    }
    transportSessionId_ = mini::net::transport::kInvalidTransportSessionId;
    return true;
}

bool PlayerSession::hasTransport() const {
    std::lock_guard lock(mutex_);
    return transportSessionId_ != mini::net::transport::kInvalidTransportSessionId;
}

std::uint64_t PlayerSession::playerId() const {
    std::lock_guard lock(mutex_);
    return playerId_;
}

std::string PlayerSession::playerName() const {
    std::lock_guard lock(mutex_);
    return playerName_;
}

std::string PlayerSession::role() const {
    std::lock_guard lock(mutex_);
    return role_;
}

std::string PlayerSession::closeReason() const {
    std::lock_guard lock(mutex_);
    return closeReason_;
}

PlayerSession::TimePoint PlayerSession::createdAt() const {
    std::lock_guard lock(mutex_);
    return createdAt_;
}

PlayerSession::TimePoint PlayerSession::lastActivityAt() const {
    std::lock_guard lock(mutex_);
    return lastActivityAt_;
}

PlayerSession::TimePoint PlayerSession::lastHeartbeatAt() const {
    std::lock_guard lock(mutex_);
    return lastHeartbeatAt_;
}

void PlayerSession::setUserContext(std::any ctx) {
    std::lock_guard lock(mutex_);
    userContext_ = std::move(ctx);
}

std::any PlayerSession::userContext() const {
    std::lock_guard lock(mutex_);
    return userContext_;
}

bool PlayerSession::hasUserContext() const {
    std::lock_guard lock(mutex_);
    return userContext_.has_value();
}

bool PlayerSession::isOnline() const {
    std::lock_guard lock(mutex_);
    return state_ == State::kOnline;
}

bool PlayerSession::isClosed() const {
    std::lock_guard lock(mutex_);
    return state_ == State::kClosed;
}

bool PlayerSession::isClosingOrClosed() const {
    std::lock_guard lock(mutex_);
    return state_ == State::kClosing || state_ == State::kClosed;
}

bool PlayerSession::isAuthenticating() const {
    std::lock_guard lock(mutex_);
    return state_ == State::kAuthenticating;
}

bool PlayerSession::isReconnecting() const {
    std::lock_guard lock(mutex_);
    return state_ == State::kReconnecting;
}

void PlayerSession::transition(State next) {
    state_ = next;
}

}  // namespace mini::game
