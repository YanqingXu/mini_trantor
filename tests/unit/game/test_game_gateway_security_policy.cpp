#include "mini/game/GameGatewaySecurityPolicy.h"

#include <cassert>
#include <chrono>
#include <stdexcept>

namespace {

template <typename Fn>
void assertInvalid(Fn&& fn) {
    try {
        fn();
        assert(false && "expected std::invalid_argument");
    } catch (const std::invalid_argument&) {
    }
}

void testDefaultPolicyIsDisabledAndValid() {
    mini::game::GameSecurityOptions options;
    options.validate();
    assert(!options.enabled());
    assert(!options.replayProtectionEnabled());
    assert(!options.sessionRateLimitEnabled());
    assert(options.authTokenNonceDelimiter == "|");
}

void testNamedSecurityResourcesEnablePolicy() {
    using namespace std::chrono_literals;

    mini::game::GameSecurityOptions tokenLimit;
    tokenLimit.maxAuthTokenBytes = 64;
    tokenLimit.validate();
    assert(tokenLimit.enabled());

    mini::game::GameSecurityOptions replay;
    replay.authReplayWindow = 500ms;
    replay.validate();
    assert(replay.enabled());
    assert(replay.replayProtectionEnabled());

    mini::game::GameSecurityOptions rateLimit;
    rateLimit.maxFramesPerSessionPerWindow = 8;
    rateLimit.sessionRateWindow = 1s;
    rateLimit.validate();
    assert(rateLimit.enabled());
    assert(rateLimit.sessionRateLimitEnabled());
}

void testDurationValidationRejectsNegativeWindows() {
    using namespace std::chrono_literals;

    mini::game::GameSecurityOptions options;
    options.authReplayWindow = -1ms;
    assertInvalid([&] { options.validate(); });

    options.authReplayWindow = 0ms;
    options.sessionRateWindow = -1ms;
    assertInvalid([&] { options.validate(); });
}

void testRateLimitRequiresPositiveWindow() {
    using namespace std::chrono_literals;

    mini::game::GameSecurityOptions options;
    options.maxFramesPerSessionPerWindow = 1;
    options.sessionRateWindow = 0ms;
    assertInvalid([&] { options.validate(); });

    options.sessionRateWindow = 1ms;
    options.validate();
}

}  // namespace

int main() {
    testDefaultPolicyIsDisabledAndValid();
    testNamedSecurityResourcesEnablePolicy();
    testDurationValidationRejectsNegativeWindows();
    testRateLimitRequiresPositiveWindow();
    return 0;
}
