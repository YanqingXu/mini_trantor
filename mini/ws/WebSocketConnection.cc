#include "mini/ws/WebSocketConnection.h"

#include "mini/net/TcpConnection.h"

namespace mini::ws {

bool WebSocketConnection::onData(const mini::net::TcpConnectionPtr& conn,
                                 mini::net::Buffer* buf) {
    while (buf->readableBytes() > 0) {
        WsFrame frame;
        std::size_t consumed = 0;
        auto result = codec::decode(buf->peek(), buf->readableBytes(), frame, consumed);

        if (result == WsDecodeResult::kIncomplete) {
            break;  // Need more data.
        }

        if (result == WsDecodeResult::kError) {
            // Protocol error: close with 1002.
            sendClose(conn, WsCloseCode::kProtocolError, "Protocol error");
            return false;
        }

        buf->retrieve(consumed);

        // Client frames MUST be masked (RFC 6455 §5.1)
        if (!frame.masked) {
            sendClose(conn, WsCloseCode::kProtocolError, "Client frame must be masked");
            return false;
        }

        // v1: require FIN=1 (no fragmentation)
        if (!frame.fin) {
            sendClose(conn, WsCloseCode::kProtocolError, "Fragmented frames not supported");
            return false;
        }

        switch (frame.opcode) {
            case WsOpcode::kText:
            case WsOpcode::kBinary:
                if (messageCallback_) {
                    messageCallback_(conn, std::move(frame.payload), frame.opcode);
                }
                break;

            case WsOpcode::kPing:
                // Auto-pong with same payload
                conn->send(codec::encodePong(frame.payload));
                break;

            case WsOpcode::kPong:
                // Ignore unsolicited pong
                break;

            case WsOpcode::kClose:
                closeReceived_ = true;
                if (!closeSent_) {
                    // Echo close frame back
                    auto code = frame.closeCode != 0
                                    ? static_cast<WsCloseCode>(frame.closeCode)
                                    : WsCloseCode::kNormal;
                    sendClose(conn, code, frame.payload);
                }
                if (closeCallback_) {
                    auto code = frame.closeCode != 0
                                    ? static_cast<WsCloseCode>(frame.closeCode)
                                    : WsCloseCode::kNoStatus;
                    closeCallback_(conn, code, frame.payload);
                }
                conn->shutdown();
                break;

            default:
                // Unknown opcode
                sendClose(conn, WsCloseCode::kProtocolError, "Unknown opcode");
                return false;
        }
    }
    return true;
}

void WebSocketConnection::sendText(const mini::net::TcpConnectionPtr& conn,
                                   std::string_view text) {
    conn->send(codec::encodeText(text));
}

void WebSocketConnection::sendBinary(const mini::net::TcpConnectionPtr& conn,
                                     std::string_view data) {
    conn->send(codec::encodeBinary(data));
}

void WebSocketConnection::sendClose(const mini::net::TcpConnectionPtr& conn,
                                    WsCloseCode code,
                                    std::string_view reason) {
    conn->send(codec::encodeClose(code, reason));
    // Note: closeSent_ is set on the per-connection WebSocketConnection object.
    // The caller should access it through the context if needed.
}

}  // namespace mini::ws
