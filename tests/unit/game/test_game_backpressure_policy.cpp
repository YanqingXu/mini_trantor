#include "mini/game/GameBackpressurePolicy.h"

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

void testDefaultDisabledPolicyIsValid() {
    mini::game::GameBackpressureOptions options;
    options.validate();
    assert(!options.enabled());
}

void testValidPolicyEnablesOnlyNamedResources() {
    using namespace std::chrono_literals;

    mini::game::GameBackpressureOptions options;
    options.input.softBufferedBytes = 1024;
    options.input.hardBufferedBytes = 2048;
    options.logic.softBacklog = 64;
    options.logic.hardBacklog = 128;
    options.logic.softOldestLag = 20ms;
    options.logic.hardOldestLag = 40ms;
    options.output.softQueuedBytes = 4096;
    options.output.hardQueuedBytes = 8192;
    options.output.softQueueLatency = 10ms;
    options.output.hardQueueLatency = 30ms;
    options.output.priority.softLimitMinPriority = mini::game::GameMessagePriority::Normal;
    options.broadcast.softFanoutConnections = 100;
    options.broadcast.hardFanoutConnections = 200;
    options.broadcast.softPayloadBytes = 32 * 1024;
    options.broadcast.hardPayloadBytes = 64 * 1024;
    options.broadcast.priority.softLimitMinPriority = mini::game::GameMessagePriority::Normal;

    options.validate();
    assert(options.enabled());
}

void testSizeLimitPairValidation() {
    mini::game::GameBackpressureOptions options;

    options.input.softBufferedBytes = 1;
    assertInvalid([&] { options.validate(); });

    options.input.hardBufferedBytes = 1;
    options.input.softBufferedBytes = 2;
    assertInvalid([&] { options.validate(); });

    options.input.softBufferedBytes = 1;
    options.validate();
}

void testDurationLimitPairValidation() {
    using namespace std::chrono_literals;

    mini::game::GameBackpressureOptions options;

    options.logic.softOldestLag = 1ms;
    assertInvalid([&] { options.validate(); });

    options.logic.hardOldestLag = 1ms;
    options.logic.softOldestLag = 2ms;
    assertInvalid([&] { options.validate(); });

    options.logic.softOldestLag = -1ms;
    options.logic.hardOldestLag = 1ms;
    assertInvalid([&] { options.validate(); });

    options.logic.softOldestLag = 1ms;
    options.logic.hardOldestLag = 2ms;
    options.validate();
}

void testEveryLayerHasIndependentValidation() {
    using namespace std::chrono_literals;

    mini::game::GameBackpressureOptions options;

    options.logic.softBacklog = 2;
    options.logic.hardBacklog = 1;
    assertInvalid([&] { options.validate(); });
    options.logic.softBacklog = 1;
    options.logic.hardBacklog = 2;

    options.output.softQueuedBytes = 8;
    options.output.hardQueuedBytes = 4;
    assertInvalid([&] { options.validate(); });
    options.output.softQueuedBytes = 4;
    options.output.hardQueuedBytes = 8;

    options.output.softQueueLatency = 6ms;
    options.output.hardQueueLatency = 3ms;
    assertInvalid([&] { options.validate(); });
    options.output.softQueueLatency = 3ms;
    options.output.hardQueueLatency = 6ms;

    options.broadcast.softFanoutConnections = 20;
    options.broadcast.hardFanoutConnections = 10;
    assertInvalid([&] { options.validate(); });
    options.broadcast.softFanoutConnections = 10;
    options.broadcast.hardFanoutConnections = 20;

    options.broadcast.softPayloadBytes = 4096;
    options.broadcast.hardPayloadBytes = 1024;
    assertInvalid([&] { options.validate(); });
    options.broadcast.softPayloadBytes = 1024;
    options.broadcast.hardPayloadBytes = 4096;

    options.validate();
}

void testPrioritySheddingRequiresNamedThresholds() {
    mini::game::GameBackpressureOptions options;

    options.output.priority.softLimitMinPriority = mini::game::GameMessagePriority::Normal;
    assertInvalid([&] { options.validate(); });

    options.output.hardQueuedBytes = 10;
    assertInvalid([&] { options.validate(); });

    options.output.priority.adaptiveSoftLimit = true;
    options.validate();

    options.output = {};
    options.broadcast.priority.softLimitMinPriority = mini::game::GameMessagePriority::Normal;
    assertInvalid([&] { options.validate(); });

    options.broadcast.hardFanoutConnections = 10;
    assertInvalid([&] { options.validate(); });

    options.broadcast.softFanoutConnections = 5;
    options.validate();
}

void testAdaptivePrioritySheddingDerivesAndEscalatesSoftThreshold() {
    mini::game::GameBackpressureOptions::PriorityShedding policy;
    policy.softLimitMinPriority = mini::game::GameMessagePriority::Normal;
    policy.adaptiveSoftLimit = true;

    assert(policy.effectiveSoftLimit(0, 100) == 50);
    assert(policy.requiredPriority(49, 0, 100) == mini::game::GameMessagePriority::Low);
    assert(policy.requiredPriority(50, 0, 100) == mini::game::GameMessagePriority::Normal);
    assert(policy.requiredPriority(75, 0, 100) == mini::game::GameMessagePriority::High);

    assert(policy.shouldDrop(
        mini::game::toMetricPriority(mini::game::GameMessagePriority::Low),
        50,
        0,
        100));
    assert(!policy.shouldDrop(
        mini::game::toMetricPriority(mini::game::GameMessagePriority::Normal),
        50,
        0,
        100));
    assert(policy.shouldDrop(
        mini::game::toMetricPriority(mini::game::GameMessagePriority::Normal),
        75,
        0,
        100));
    assert(!policy.shouldDrop(
        mini::game::toMetricPriority(mini::game::GameMessagePriority::High),
        75,
        0,
        100));
}

void testPacketFlagPriorityMappingKeepsDefaultNormal() {
    assert(mini::game::priorityFromPacketFlags(0) == mini::game::GameMessagePriority::Normal);
    assert(mini::game::priorityFromPacketFlags(mini::game::kGamePriorityFlagLow) ==
           mini::game::GameMessagePriority::Low);
    assert(mini::game::priorityFromPacketFlags(mini::game::kGamePriorityFlagHigh) ==
           mini::game::GameMessagePriority::High);
    assert(mini::game::priorityFromPacketFlags(mini::game::kGamePriorityFlagCritical) ==
           mini::game::GameMessagePriority::Critical);
}

}  // namespace

int main() {
    testDefaultDisabledPolicyIsValid();
    testValidPolicyEnablesOnlyNamedResources();
    testSizeLimitPairValidation();
    testDurationLimitPairValidation();
    testEveryLayerHasIndependentValidation();
    testPrioritySheddingRequiresNamedThresholds();
    testAdaptivePrioritySheddingDerivesAndEscalatesSoftThreshold();
    testPacketFlagPriorityMappingKeepsDefaultNormal();
    return 0;
}
