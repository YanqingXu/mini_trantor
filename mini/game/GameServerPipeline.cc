#include "mini/game/GameServerPipeline.h"

#include "mini/game/SessionManager.h"
#include "mini/game/logic/LogicLoop.h"
#include "mini/net/Buffer.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"
#include "mini/net/transport/TransportEndpoint.h"
#include "mini/net/transport/TransportManager.h"

#include <any>
#include <array>
#include <memory>
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
}

void GameServerPipeline::install() {
    server_.setConnectionCallback([this](const std::shared_ptr<net::TcpConnection>& connection) {
        onConnection(connection);
    });
    server_.setMessageCallback([this](const std::shared_ptr<net::TcpConnection>& connection, net::Buffer* buffer) {
        onMessage(connection, buffer);
    });
}

const GameServerPipeline::Options& GameServerPipeline::options() const noexcept {
    return options_;
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
            server_.unbindBroadcastSession(state->sessionToken);
            sessionManager_.onConnectionClose(state->transportSessionId, "network close");
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
        connection->forceClose();
        return;
    }
    auto state = *statePtr;
    state->input += buffer->retrieveAllAsString();

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
            connection->forceClose();
            return;
        }

        handleFrame(connection, state, packet);
        offset += consumed;
        ++frames;
    }

    if (offset > 0) {
        state->input.erase(0, offset);
    }
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
        if (payload.empty()) {
            connection->forceClose();
            return;
        }

        state->sessionToken = payload;
        auto session = sessionManager_.ensureSession(payload, state->transportSessionId, false);
        if (!session) {
            connection->forceClose();
            return;
        }

        sessionManager_.bindTransportEndpoint(payload, state->endpoint);
        if (session->state() == PlayerSession::State::kCreated) {
            sessionManager_.markStartAuth(payload);
        }
        if (session->state() == PlayerSession::State::kAuthenticating) {
            sessionManager_.authenticate(payload, state->transportSessionId, payload, "default");
        }
        sessionManager_.markOnline(payload);

        server_.bindBroadcastSession(connection, payload);
        server_.joinBroadcastGroup(payload, options_.defaultGroup);
        server_.joinBroadcastAoi(payload, options_.defaultAoi);

        state->authenticated = true;
        state->endpoint->send(state->framer.encode(
            options_.responseMsgId,
            0,
            packet.header.seq,
            "auth-ok"));
        return;
    }

    if (!state->authenticated) {
        connection->forceClose();
        return;
    }

    if (packet.header.msgId == options_.commandMsgId) {
        logicLoop_.submit(
            state->sessionToken,
            state->transportSessionId,
            state->endpoint,
            payload);
        return;
    }

    if (packet.header.msgId == options_.broadcastMsgId) {
        server_.broadcastGroup(
            options_.defaultGroup,
            state->framer.encode(options_.responseMsgId, 0, packet.header.seq, "broadcast:" + payload));
        return;
    }
}

}  // namespace mini::game
