#include "mini/base/MetricsHook.h"
#include "mini/game/GameServerPipeline.h"

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

void testPipelineOptionsValidateGameBackpressurePolicy() {
    mini::game::GameServerPipeline::Options options;
    options.validate();
    assert(!options.backpressure.enabled());

    options.backpressure.input.softBufferedBytes = 1024;
    options.backpressure.input.hardBufferedBytes = 4096;
    options.backpressure.logic.softBacklog = 128;
    options.backpressure.logic.hardBacklog = 256;
    options.backpressure.output.softQueuedBytes = 8192;
    options.backpressure.output.hardQueuedBytes = 16384;
    options.backpressure.broadcast.softFanoutConnections = 1000;
    options.backpressure.broadcast.hardFanoutConnections = 2000;
    options.validate();
    assert(options.backpressure.enabled());

    options.backpressure.broadcast.softPayloadBytes = 4096;
    assertInvalid([&] { options.validate(); });
}

void testGameBackpressureMetricSchemaNamesDecisionContext() {
    bool observed = false;
    mini::game::GameBackpressureMetricCallback callback =
        [&](const mini::game::GameBackpressureMetricSample& sample) {
            observed = true;
            assert(sample.event == mini::game::GameBackpressureMetricEvent::InputRejected);
            assert(sample.layer == mini::game::GameBackpressureLayer::InputFraming);
            assert(sample.action == mini::game::GameBackpressureAction::Close);
            assert(sample.reason == mini::game::GameBackpressureReason::InputBufferedBytesHardLimit);
            assert(sample.sessionToken == "session-1");
            assert(sample.transportSessionId == 42);
            assert(sample.msgId == 7);
            assert(sample.currentValue == 4097);
            assert(sample.softLimit == 1024);
            assert(sample.hardLimit == 4096);
        };

    mini::game::GameBackpressureMetricSample sample;
    sample.event = mini::game::GameBackpressureMetricEvent::InputRejected;
    sample.layer = mini::game::GameBackpressureLayer::InputFraming;
    sample.action = mini::game::GameBackpressureAction::Close;
    sample.reason = mini::game::GameBackpressureReason::InputBufferedBytesHardLimit;
    sample.sessionToken = "session-1";
    sample.transportSessionId = 42;
    sample.msgId = 7;
    sample.currentValue = 4097;
    sample.softLimit = 1024;
    sample.hardLimit = 4096;

    callback(sample);
    assert(observed);
}

void testGameBackpressureMetricSchemaCoversBroadcastDecision() {
    bool observed = false;
    mini::game::GameBackpressureMetricCallback callback =
        [&](const mini::game::GameBackpressureMetricSample& sample) {
            observed = true;
            assert(sample.event == mini::game::GameBackpressureMetricEvent::BroadcastRejected);
            assert(sample.layer == mini::game::GameBackpressureLayer::BroadcastFanout);
            assert(sample.action == mini::game::GameBackpressureAction::Reject);
            assert(sample.reason == mini::game::GameBackpressureReason::BroadcastFanoutHardLimit);
            assert(sample.fanoutConnections == 5001);
            assert(sample.payloadBytes == 256);
            assert(sample.hardLimit == 5000);
        };

    mini::game::GameBackpressureMetricSample sample;
    sample.event = mini::game::GameBackpressureMetricEvent::BroadcastRejected;
    sample.layer = mini::game::GameBackpressureLayer::BroadcastFanout;
    sample.action = mini::game::GameBackpressureAction::Reject;
    sample.reason = mini::game::GameBackpressureReason::BroadcastFanoutHardLimit;
    sample.fanoutConnections = 5001;
    sample.payloadBytes = 256;
    sample.hardLimit = 5000;

    callback(sample);
    assert(observed);
}

}  // namespace

int main() {
    testPipelineOptionsValidateGameBackpressurePolicy();
    testGameBackpressureMetricSchemaNamesDecisionContext();
    testGameBackpressureMetricSchemaCoversBroadcastDecision();
    return 0;
}
