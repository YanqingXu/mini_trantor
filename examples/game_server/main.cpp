#include "mini/game/GameServerPipeline.h"
#include "mini/game/SessionManager.h"
#include "mini/game/logic/LogicLoop.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/SignalWatcher.h"
#include "mini/net/TcpServer.h"
#include "mini/net/framing/PacketFramer.h"
#include "mini/net/transport/TransportManager.h"

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::mutex gLogMutex;

template <typename... Args>
void logLine(Args&&... args) {
    std::scoped_lock lock(gLogMutex);
    (std::cout << ... << std::forward<Args>(args)) << '\n';
}

mini::game::GameServerPipeline::Options makePipelineOptions() {
    mini::game::GameServerPipeline::Options options;
    options.backpressure.input.softBufferedBytes = 128 * 1024;
    options.backpressure.input.hardBufferedBytes = 256 * 1024;
    options.backpressure.logic.softBacklog = 4096;
    options.backpressure.logic.hardBacklog = 8192;
    options.backpressure.output.softQueuedBytes = 128 * 1024;
    options.backpressure.output.hardQueuedBytes = 256 * 1024;
    options.backpressure.broadcast.softFanoutConnections = 2048;
    options.backpressure.broadcast.hardFanoutConnections = 4096;
    options.backpressure.broadcast.softPayloadBytes = 32 * 1024;
    options.backpressure.broadcast.hardPayloadBytes = 64 * 1024;
    options.security.maxAuthTokenBytes = 128;
    options.security.authReplayWindow = 30s;
    options.security.maxFramesPerSessionPerWindow = 512;
    options.security.sessionRateWindow = 1s;
    return options;
}

const char* securityEventName(mini::game::GameSecurityMetricEvent event) {
    switch (event) {
    case mini::game::GameSecurityMetricEvent::AuthAccepted:
        return "auth-accepted";
    case mini::game::GameSecurityMetricEvent::AuthRejected:
        return "auth-rejected";
    case mini::game::GameSecurityMetricEvent::RateLimited:
        return "rate-limited";
    case mini::game::GameSecurityMetricEvent::AbnormalClose:
        return "abnormal-close";
    }
    return "unknown";
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::uint16_t port = argc > 1
        ? static_cast<std::uint16_t>(std::stoi(argv[1]))
        : 8890;
    const int workerThreads = argc > 2 ? std::stoi(argv[2]) : 0;

    mini::net::SignalWatcher::blockSignals();

    mini::net::EventLoop loop;
    mini::net::SignalWatcher signalWatcher(&loop);
    mini::net::transport::TransportManager transportManager(&loop);
    mini::game::SessionManager sessionManager(&loop);
    const auto pipelineOptions = makePipelineOptions();
    mini::game::logic::LogicLoop logicLoop(
        {.fixedStep = 16ms,
         .maxCommandsPerTick = 128,
         .admission = pipelineOptions.backpressure.logic,
         .output = pipelineOptions.backpressure.output});

    mini::net::TcpServer server(&loop, mini::net::InetAddress(port), "game_server");
    server.setThreadNum(workerThreads);

    mini::game::GameServerPipeline pipeline(
        server,
        transportManager,
        sessionManager,
        logicLoop,
        pipelineOptions);

    pipeline.setAuthTokenValidator([](std::string_view sessionToken, std::string_view nonce) {
        return !sessionToken.empty() && !nonce.empty() && sessionToken.size() <= 64;
    });

    logicLoop.setProcessor([](const mini::game::logic::GameCommand& command,
                              std::vector<mini::game::logic::GameCommand>& outputs) {
        mini::net::framing::PacketFramer framer;
        outputs.emplace_back(command.sessionId,
                             command.transportSessionId,
                             command.sourceTransport,
                             framer.encode(4, 0, 0, "logic:" + command.payload),
                             command.priority);
    });

    pipeline.setMetricCallback([](const mini::game::GamePipelineMetricSample& sample) {
        if (sample.event == mini::game::GamePipelineMetricEvent::LogicSubmitResult) {
            logLine("logic-submit session=", sample.sessionToken,
                    " msg=", sample.msgId,
                    " accepted=", sample.logicSubmitted,
                    " backlog=", sample.logicBacklog);
        }
    });
    pipeline.setSecurityMetricCallback([](const mini::game::GameSecurityMetricSample& sample) {
        logLine("security event=", securityEventName(sample.event),
                " session=", sample.sessionToken,
                " transport=", sample.transportSessionId,
                " msg=", sample.msgId);
    });

    pipeline.install();

    signalWatcher.setSignalCallback([&](int signum) {
        logLine("signal ", signum, " received, stopping game_server");
        server.stop();
        logicLoop.stop();
        loop.quit();
    });

    logicLoop.start();
    server.start();

    logLine("game_server listening on 0.0.0.0:", port,
            " with ", workerThreads, " worker threads");
    logLine("framed msg ids: auth=1 command=2 broadcast=3 response=4");
    logLine("auth payload accepts either <session> or <session>|<nonce>");

    loop.loop();

    server.stop();
    logicLoop.stop();
    return EXIT_SUCCESS;
}
