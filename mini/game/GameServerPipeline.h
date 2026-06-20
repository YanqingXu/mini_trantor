#pragma once

// GameServerPipeline — 游戏服务器默认网络切片绑定器。
//
// 它不拥有 TcpServer / SessionManager / LogicLoop / TransportManager；
// 只负责把 TCP framed packet -> session auth -> logic command -> owner-loop send
// 这条最小默认路径接起来，供示例和集成测试复用。

#include "mini/base/noncopyable.h"
#include "mini/net/framing/PacketFramer.h"
#include "mini/net/transport/TransportTypes.h"

#include <cstdint>
#include <memory>
#include <string>

namespace mini::net {
class TcpServer;
class TcpConnection;
class Buffer;
namespace transport {
class ITransportEndpoint;
class TransportManager;
}
}

namespace mini::game {
class SessionManager;
namespace logic {
class LogicLoop;
}

class GameServerPipeline : private mini::base::noncopyable {
public:
    struct Options {
        std::uint32_t authMsgId{1};
        std::uint32_t commandMsgId{2};
        std::uint32_t broadcastMsgId{3};
        std::uint32_t responseMsgId{4};
        std::string defaultGroup{"room:default"};
        std::string defaultAoi{"aoi:default"};
    };

    GameServerPipeline(net::TcpServer& server,
                       net::transport::TransportManager& transportManager,
                       SessionManager& sessionManager,
                       logic::LogicLoop& logicLoop);
    GameServerPipeline(net::TcpServer& server,
                       net::transport::TransportManager& transportManager,
                       SessionManager& sessionManager,
                       logic::LogicLoop& logicLoop,
                       Options options);

    void install();

    const Options& options() const noexcept;

private:
    struct ConnectionState {
        net::transport::TransportSessionId transportSessionId{
            net::transport::kInvalidTransportSessionId};
        std::shared_ptr<net::transport::ITransportEndpoint> endpoint;
        net::framing::PacketFramer framer;
        std::string input;
        std::string sessionToken;
        bool authenticated{false};
    };

    void onConnection(const std::shared_ptr<net::TcpConnection>& connection);
    void onMessage(const std::shared_ptr<net::TcpConnection>& connection, net::Buffer* buffer);
    void handleFrame(const std::shared_ptr<net::TcpConnection>& connection,
                     const std::shared_ptr<ConnectionState>& state,
                     const net::framing::Packet& packet);

    net::TcpServer& server_;
    net::transport::TransportManager& transportManager_;
    SessionManager& sessionManager_;
    logic::LogicLoop& logicLoop_;
    Options options_;
};

}  // namespace mini::game
