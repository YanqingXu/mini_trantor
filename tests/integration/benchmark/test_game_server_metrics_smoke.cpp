#include "mini/base/MetricsExporter.h"
#include "mini/game/GameServerPipeline.h"
#include "mini/game/SessionManager.h"
#include "mini/game/logic/LogicLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpServer.h"
#include "mini/net/TcpServerOptions.h"
#include "mini/net/framing/PacketFramer.h"
#include "mini/net/transport/TransportManager.h"

#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

uint16_t allocateTestPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    require(fd >= 0, "failed to create test socket");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    require(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0,
            "failed to bind test socket");

    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    require(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0,
            "failed to read test socket port");
    const auto port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

struct DecodedFrame {
    std::uint32_t msgId{0};
    std::uint32_t seq{0};
    std::string payload;
};

class FramedClient {
public:
    explicit FramedClient(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        require(fd_ >= 0, "client socket failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        require(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1,
                "client address conversion failed");
        require(::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0,
                "client connect failed");

        timeval timeout{};
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;
        require(::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
                "client receive timeout setup failed");
    }

    ~FramedClient() {
        closeNow();
    }

    void sendFrame(std::uint32_t msgId, std::uint32_t seq, std::string_view payload) {
        sendRaw(framer_.encode(msgId, 0, seq, payload));
    }

    void sendRaw(std::string_view bytes) {
        while (!bytes.empty()) {
            const auto n = ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            require(n > 0, "client send failed");
            bytes.remove_prefix(static_cast<std::size_t>(n));
        }
    }

    DecodedFrame readFrame() {
        for (;;) {
            mini::net::framing::Packet packet;
            std::size_t consumed = 0;
            const auto state = framer_.decode(input_.data(), input_.size(), packet, consumed);
            if (state == mini::net::framing::PacketDecodeState::kComplete) {
                DecodedFrame decoded{
                    packet.header.msgId,
                    packet.header.seq,
                    std::string(packet.payload)};
                input_.erase(0, consumed);
                return decoded;
            }
            require(state == mini::net::framing::PacketDecodeState::kNeedMore,
                    "client decoded invalid frame");

            char buffer[512]{};
            const auto n = ::recv(fd_, buffer, sizeof(buffer), 0);
            require(n > 0, "client receive failed before a complete frame arrived");
            input_.append(buffer, static_cast<std::size_t>(n));
        }
    }

    void closeNow() {
        if (fd_ >= 0) {
            (void)::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_{-1};
    mini::net::framing::PacketFramer framer_;
    std::string input_;
};

struct MetricProbe {
    std::mutex mutex;
    std::condition_variable cv;
    std::string failure;
    std::size_t pipelineInputBatches{0};
    std::size_t pipelineContinuations{0};
    std::size_t pipelineLogicSubmits{0};
    std::size_t logicEnqueued{0};
    std::size_t logicTicksWithDrain{0};
    std::size_t logicOutputDispatched{0};
    std::size_t logicOutputSent{0};
    std::size_t sessionAsyncDrains{0};
    std::size_t broadcastRouted{0};
    std::size_t broadcastFlushed{0};
};

void failMetric(MetricProbe& probe, const char* message) {
    if (probe.failure.empty()) {
        probe.failure = message;
    }
}

bool metricsReady(const MetricProbe& probe) {
    return probe.pipelineInputBatches >= 3 &&
           probe.pipelineContinuations >= 1 &&
           probe.pipelineLogicSubmits >= 1 &&
           probe.logicEnqueued >= 1 &&
           probe.logicTicksWithDrain >= 1 &&
           probe.logicOutputDispatched >= 1 &&
           probe.logicOutputSent >= 1 &&
           probe.sessionAsyncDrains >= 1 &&
           probe.broadcastRouted >= 1 &&
           probe.broadcastFlushed >= 1;
}

}  // namespace

int main() {
    const auto port = allocateTestPort();

    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();

    mini::net::transport::TransportManager transportManager(baseLoop);
    mini::game::SessionManager sessionManager(baseLoop);
    mini::game::logic::LogicLoop logicLoop(
        {.fixedStep = std::chrono::milliseconds(4), .maxCommandsPerTick = 32});

    mini::net::TcpServerOptions options;
    options.numThreads = 1;
    options.metrics.enableBroadcastMetrics = true;
    auto server = std::make_unique<mini::net::TcpServer>(
        baseLoop,
        mini::net::InetAddress(port, true),
        "game-server-metrics-smoke",
        options);
    auto* serverRaw = server.get();

    MetricProbe probe;
    const mini::base::MetricLabels metricLabels{
        {"service", "game-server"},
        {"test", "metrics_smoke"},
    };
    auto exporter = std::make_shared<mini::base::InMemoryMetricsExporter>();
    auto taggedExporter = std::make_shared<mini::base::TaggedMetricsExporter>(
        exporter,
        metricLabels);
    auto metricName = [&](std::string_view name) {
        return mini::base::metricNameWithLabels(name, metricLabels);
    };
    mini::base::MetricsHookRecorder recorder(taggedExporter);
    auto broadcastRecorder = recorder.makeBroadcastCallback();
    auto sessionRecorder = recorder.makeSessionCallback();
    auto logicRecorder = recorder.makeLogicLoopCallback();
    auto pipelineRecorder = recorder.makeGamePipelineCallback();

    serverRaw->setBroadcastMetricCallback(
        [&, broadcastRecorder](const mini::net::BroadcastMetricSample& sample) {
            broadcastRecorder(sample);
            std::lock_guard lock(probe.mutex);
            if (!sample.loop) {
                failMetric(probe, "broadcast metric loop is null");
            }
            if (sample.payloadBytes == 0) {
                failMetric(probe, "broadcast metric payload is empty");
            }
            if (sample.event == mini::net::BroadcastMetricEvent::Routed) {
                if (sample.loop != baseLoop) {
                    failMetric(probe, "broadcast routed metric did not run on base loop");
                }
                if (sample.fanoutConnections == 0) {
                    failMetric(probe, "broadcast routed fanout is empty");
                }
                if (sample.routeLatency < mini::net::BroadcastMetricSample::Duration::zero()) {
                    failMetric(probe, "broadcast route latency is negative");
                }
                ++probe.broadcastRouted;
            } else if (sample.event == mini::net::BroadcastMetricEvent::LoopFlushed) {
                if (sample.fanoutConnections == 0) {
                    failMetric(probe, "broadcast flushed fanout is empty");
                }
                if (sample.queueLatency < mini::net::BroadcastMetricSample::Duration::zero()) {
                    failMetric(probe, "broadcast queue latency is negative");
                }
                if (sample.fanoutLatency < mini::net::BroadcastMetricSample::Duration::zero()) {
                    failMetric(probe, "broadcast fanout latency is negative");
                }
                ++probe.broadcastFlushed;
            }
            probe.cv.notify_all();
        });

    sessionManager.setMetricCallback(
        [&, sessionRecorder](const mini::game::SessionMetricSample& sample) {
            sessionRecorder(sample);
            std::lock_guard lock(probe.mutex);
            if (sample.event == mini::game::SessionMetricEvent::AsyncEventsDrained) {
                if (sample.loop != baseLoop) {
                    failMetric(probe, "session async drain did not run on owner loop");
                }
                if (sample.drainedEvents == 0 || sample.pendingEvents == 0) {
                    failMetric(probe, "session async drain count is empty");
                }
                if (sample.oldestEventLag < mini::game::SessionMetricSample::Duration::zero()) {
                    failMetric(probe, "session async event lag is negative");
                }
                ++probe.sessionAsyncDrains;
            }
            probe.cv.notify_all();
        });

    logicLoop.setMetricCallback(
        [&, logicRecorder](const mini::game::logic::LogicLoopMetricSample& sample) {
            logicRecorder(sample);
            std::lock_guard lock(probe.mutex);
            if (!sample.loop) {
                failMetric(probe, "logic metric loop is null");
            }
            switch (sample.event) {
                case mini::game::logic::LogicLoopMetricEvent::CommandEnqueued:
                    ++probe.logicEnqueued;
                    break;
                case mini::game::logic::LogicLoopMetricEvent::TickCompleted:
                    if (sample.drainedCommands > 0) {
                        ++probe.logicTicksWithDrain;
                    }
                    break;
                case mini::game::logic::LogicLoopMetricEvent::OutputDispatched:
                    if (sample.outputBatch == 0 || sample.queuedOutputs == 0) {
                        failMetric(probe, "logic output dispatch did not queue output");
                    }
                    if (sample.outputBytes == 0) {
                        failMetric(probe, "logic output dispatch bytes are empty");
                    }
                    ++probe.logicOutputDispatched;
                    break;
                case mini::game::logic::LogicLoopMetricEvent::OutputSent:
                    if (sample.outputBytes == 0) {
                        failMetric(probe, "logic output sent bytes are empty");
                    }
                    if (sample.outputQueueLatency <
                        mini::game::logic::LogicLoopMetricSample::Duration::zero()) {
                        failMetric(probe, "logic output queue latency is negative");
                    }
                    ++probe.logicOutputSent;
                    break;
            }
            probe.cv.notify_all();
        });

    logicLoop.setProcessor([](const mini::game::logic::GameCommand& command,
                              std::vector<mini::game::logic::GameCommand>& outputs) {
        mini::net::framing::PacketFramer framer;
        outputs.emplace_back(command.sessionId,
                             command.transportSessionId,
                             command.sourceTransport,
                             framer.encode(4, 0, 0, "logic:" + command.payload));
    });

    mini::game::GameServerPipeline pipeline(*serverRaw, transportManager, sessionManager, logicLoop);
    pipeline.setMetricCallback(
        [&, pipelineRecorder](const mini::game::GamePipelineMetricSample& sample) {
            pipelineRecorder(sample);
            std::lock_guard lock(probe.mutex);
            if (!sample.loop) {
                failMetric(probe, "pipeline metric loop is null");
            }
            if (sample.event == mini::game::GamePipelineMetricEvent::InputBatchProcessed) {
                if (sample.framesDecoded > 0) {
                    if (sample.bytesConsumed == 0) {
                        failMetric(probe, "pipeline input consumed zero bytes");
                    }
                    if (sample.batchDuration <
                        mini::game::GamePipelineMetricSample::Duration::zero()) {
                        failMetric(probe, "pipeline batch duration is negative");
                    }
                    ++probe.pipelineInputBatches;
                }
                if (sample.continuationScheduled) {
                    if (sample.framesDecoded != mini::net::framing::kDefaultMaxFramesPerBatch) {
                        failMetric(probe, "pipeline continuation did not hit frame batch limit");
                    }
                    if (sample.bufferedBytes == 0) {
                        failMetric(probe, "pipeline continuation has no buffered bytes");
                    }
                    ++probe.pipelineContinuations;
                }
            } else if (sample.event == mini::game::GamePipelineMetricEvent::LogicSubmitResult) {
                if (sample.sessionToken == "bench-player") {
                    if (sample.msgId != 2 || !sample.logicSubmitted) {
                        failMetric(probe, "pipeline logic submit result is invalid");
                    }
                    ++probe.pipelineLogicSubmits;
                }
            }
            probe.cv.notify_all();
        });
    pipeline.install();

    auto started = std::make_shared<std::promise<void>>();
    auto startedFuture = started->get_future();
    baseLoop->queueInLoop([serverRaw, started] {
        serverRaw->start();
        started->set_value();
    });
    require(startedFuture.wait_for(2s) == std::future_status::ready,
            "server did not start before timeout");

    logicLoop.start();

    FramedClient client(port);
    client.sendFrame(1, 1, "bench-player");
    const auto auth = client.readFrame();
    require(auth.msgId == 4, "auth response msg id mismatch");
    require(auth.seq == 1, "auth response seq mismatch");
    require(auth.payload == "auth-ok", "auth response payload mismatch");

    sessionManager.postRefreshHeartbeat("bench-player");

    client.sendFrame(2, 2, "move");
    const auto logic = client.readFrame();
    require(logic.msgId == 4, "logic response msg id mismatch");
    require(logic.payload == "logic:move", "logic response payload mismatch");

    client.sendFrame(3, 3, "hello");
    const auto broadcast = client.readFrame();
    require(broadcast.msgId == 4, "broadcast response msg id mismatch");
    require(broadcast.seq == 3, "broadcast response seq mismatch");
    require(broadcast.payload == "broadcast:hello", "broadcast response payload mismatch");

    mini::net::framing::PacketFramer burstFramer;
    std::string burst;
    constexpr auto kBurstCount =
        static_cast<int>(mini::net::framing::kDefaultMaxFramesPerBatch * 4);
    for (int i = 0; i < kBurstCount; ++i) {
        burst += burstFramer.encode(3, 0, static_cast<std::uint32_t>(100 + i),
                                    "burst-" + std::to_string(i));
    }
    client.sendRaw(burst);
    for (int i = 0; i < kBurstCount; ++i) {
        const auto reply = client.readFrame();
        require(reply.msgId == 4, "burst response msg id mismatch");
        require(reply.seq == static_cast<std::uint32_t>(100 + i),
                "burst response seq mismatch");
        require(reply.payload == "broadcast:burst-" + std::to_string(i),
                "burst response payload mismatch");
    }

    {
        std::unique_lock lock(probe.mutex);
        const bool ready = probe.cv.wait_for(lock, 3s, [&] {
            return !probe.failure.empty() || metricsReady(probe);
        });
        if (!probe.failure.empty()) {
            throw std::runtime_error(probe.failure);
        }
        require(ready, "game server metrics smoke did not observe the expected metric set");
    }
    require(exporter->counterValue(metricName("mini.game.pipeline.input_batch_processed")) >=
                probe.pipelineInputBatches,
            "metrics exporter missed pipeline input batches");
    require(exporter->counterValue(metricName("mini.game.pipeline.logic_submit_result")) >=
                probe.pipelineLogicSubmits,
            "metrics exporter missed pipeline logic submits");
    require(exporter->counterValue(metricName("mini.game.logic.command_enqueued")) >=
                probe.logicEnqueued,
            "metrics exporter missed logic enqueue events");
    require(exporter->counterValue(metricName("mini.game.logic.output_dispatched")) >=
                probe.logicOutputDispatched,
            "metrics exporter missed logic output dispatch events");
    require(exporter->counterValue(metricName("mini.game.session.async_events_drained")) >=
                probe.sessionAsyncDrains,
            "metrics exporter missed session async drain events");
    require(exporter->counterValue(metricName("mini.net.broadcast.routed")) >=
                probe.broadcastRouted,
            "metrics exporter missed broadcast routed events");
    require(exporter->counterValue(metricName("mini.net.broadcast.loop_flushed")) >=
                probe.broadcastFlushed,
            "metrics exporter missed broadcast flush events");
    require(exporter->histogram(metricName("mini.game.logic.tick_duration_ms")).count > 0,
            "metrics exporter missed logic tick duration histogram");
    require(exporter->histogram(metricName("mini.net.broadcast.payload_bytes")).max > 0.0,
            "metrics exporter missed broadcast payload histogram");
    const auto prometheus = mini::base::renderPrometheusText(exporter->snapshot());
    require(prometheus.find(
                "mini_game_pipeline_input_batch_processed{service=\"game-server\",test=\"metrics_smoke\"}") !=
                std::string::npos,
            "prometheus renderer missed tagged pipeline counter");
    require(prometheus.find(
                "mini_net_broadcast_payload_bytes_count{service=\"game-server\",test=\"metrics_smoke\"}") !=
                std::string::npos,
            "prometheus renderer missed tagged broadcast histogram");

    client.closeNow();
    logicLoop.stop();

    auto stopped = std::make_shared<std::promise<void>>();
    auto stoppedFuture = stopped->get_future();
    baseLoop->queueInLoop([serverRaw, stopped] {
        serverRaw->stop();
        stopped->set_value();
    });
    require(stoppedFuture.wait_for(2s) == std::future_status::ready,
            "server did not stop before timeout");

    auto serverOwner = std::make_shared<std::unique_ptr<mini::net::TcpServer>>(std::move(server));
    auto destroyed = std::make_shared<std::promise<void>>();
    auto destroyedFuture = destroyed->get_future();
    baseLoop->queueInLoop([serverOwner, baseLoop, destroyed] {
        serverOwner->reset();
        baseLoop->quit();
        destroyed->set_value();
    });
    require(destroyedFuture.wait_for(2s) == std::future_status::ready,
            "server did not destroy before timeout");

    return 0;
}
