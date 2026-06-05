#pragma once

// WebSocketCodec 是无状态的 WebSocket 帧编解码工具。
// 遵循 RFC 6455 §5：opcode、masking、7/16/64-bit 负载长度。
// 服务端发送帧不掩码；客户端帧必须掩码。
// 无线程亲和性，纯工具函数。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mini::ws {

enum class WsOpcode : std::uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA,
};

enum class WsCloseCode : std::uint16_t {
    kNormal = 1000,
    kGoingAway = 1001,
    kProtocolError = 1002,
    kUnsupportedData = 1003,
    kNoStatus = 1005,
    kAbnormal = 1006,
    kInvalidPayload = 1007,
    kPolicyViolation = 1008,
    kTooLarge = 1009,
    kMandatoryExtension = 1010,
    kInternalError = 1011,
};

/// Decoded frame from the wire.
struct WsFrame {
    bool fin{true};
    WsOpcode opcode{WsOpcode::kText};
    bool masked{false};
    std::string payload;
    std::uint16_t closeCode{0};  // Only valid for opcode == kClose
};

/// Result of a decode attempt.
enum class WsDecodeResult {
    kComplete,     // A full frame was decoded
    kIncomplete,   // Need more data
    kError,        // Malformed frame
};

namespace codec {

/// Encode a WebSocket frame (server-to-client, unmasked).
/// @param fin    Whether this is the final fragment
/// @param opcode The frame opcode
/// @param payload The payload data
/// @return The encoded frame bytes
std::string encode(bool fin, WsOpcode opcode, std::string_view payload);

/// Convenience: encode a text message frame.
inline std::string encodeText(std::string_view text) {
    return encode(true, WsOpcode::kText, text);
}

/// Convenience: encode a binary message frame.
inline std::string encodeBinary(std::string_view data) {
    return encode(true, WsOpcode::kBinary, data);
}

/// Convenience: encode a ping frame.
inline std::string encodePing(std::string_view payload = {}) {
    return encode(true, WsOpcode::kPing, payload);
}

/// Convenience: encode a pong frame.
inline std::string encodePong(std::string_view payload = {}) {
    return encode(true, WsOpcode::kPong, payload);
}

/// Convenience: encode a close frame with optional status code and reason.
std::string encodeClose(WsCloseCode code = WsCloseCode::kNormal,
                        std::string_view reason = {});

/// Decode a WebSocket frame from raw bytes.
/// @param data   Pointer to the beginning of frame data
/// @param len    Number of available bytes
/// @param[out] frame  The decoded frame (valid only if kComplete)
/// @param[out] consumed  Number of bytes consumed (valid only if kComplete)
/// @return Decode result
WsDecodeResult decode(const char* data, std::size_t len,
                      WsFrame& frame, std::size_t& consumed);

/// Unmask payload in-place using the 4-byte mask key.
void unmask(char* data, std::size_t len, const std::uint8_t maskKey[4]);

/// Maximum payload size before we reject the frame (v1 safety limit).
static constexpr std::size_t kMaxPayloadSize = 65536;

}  // namespace codec
}  // namespace mini::ws
