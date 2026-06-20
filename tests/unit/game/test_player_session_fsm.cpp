#include "mini/game/PlayerSession.h"

#include <any>
#include <cassert>
#include <chrono>

int main() {
    using namespace std::chrono_literals;
    using namespace mini::game;

    // 1) Constructor, transport bind and user context.
    PlayerSession session("token-auth", 1001, 5ms, 20ms);
    assert(session.sessionId() == "token-auth");
    assert(session.transportSessionId() == 1001);
    assert(session.state() == PlayerSession::State::kCreated);
    assert(!session.isOnline() && !session.isClosed());

    session.setUserContext(std::string("ctx"));
    assert(session.hasUserContext());
    const auto& userContext = session.userContext();
    assert(std::any_cast<std::string>(&userContext) != nullptr);

    // 2) Authentication state flow.
    assert(session.startAuthentication());
    assert(session.state() == PlayerSession::State::kAuthenticating);
    assert(!session.startAuthentication()); // idempotent
    assert(!session.onAuthTimeout(PlayerSession::TimePoint{}));
    const auto now = std::chrono::steady_clock::now();
    assert(session.onAuthTimeout(now + std::chrono::seconds(10)));
    assert(session.state() == PlayerSession::State::kClosed);

    // 3) Heartbeat flow after re-open for heartbeat checks.
    PlayerSession heartbeatSession("token-hb", 2002, 5ms, 10ms);
    assert(heartbeatSession.startAuthentication());
    assert(heartbeatSession.markAuthenticated(11, "hero", "mage"));
    assert(heartbeatSession.markOnline());
    assert(heartbeatSession.isOnline());
    assert(!heartbeatSession.onHeartbeatTimeout(now + 9ms));
    assert(heartbeatSession.onHeartbeatTimeout(now + 30ms));
    assert(heartbeatSession.state() == PlayerSession::State::kHeartbeatTimeout);
    assert(heartbeatSession.refreshHeartbeat());
    assert(heartbeatSession.state() == PlayerSession::State::kOnline);

    // 4) close / reconnecting transitions.
    assert(heartbeatSession.onConnectionClose("disconnect"));
    assert(heartbeatSession.state() == PlayerSession::State::kClosing);
    assert(!heartbeatSession.onConnectionClose("again"));
    assert(heartbeatSession.markReconnecting());
    assert(heartbeatSession.state() == PlayerSession::State::kReconnecting);
    assert(heartbeatSession.markOnline());
    assert(heartbeatSession.state() == PlayerSession::State::kOnline);
    assert(heartbeatSession.close("manual"));
    assert(heartbeatSession.isClosed());
    assert(!heartbeatSession.close("again"));

    // 5) transport binding.
    assert(session.transportSessionId() == 1001);
    assert(session.detachTransport());
    assert(!session.hasTransport());
    assert(!session.detachTransport());
    // Closed sessions should not be able to rebind transport id.
    assert(!session.bindTransportSession(3003));
    // Valid rebind when session is alive.
    PlayerSession bindable("token-bind", 4004, 5ms, 20ms);
    assert(bindable.startAuthentication());
    assert(bindable.close("manual"));
    assert(!bindable.bindTransportSession(5005));

    return 0;
}
