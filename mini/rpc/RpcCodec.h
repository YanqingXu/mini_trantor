#pragma once

// RpcCodec 是无状态的 RPC 帧编解码工具。
// 帧被 PacketFramer 再封装为统一协议（magic + len + msgId + flags + seq + payload）：
// payload 内部保持 RPC 原始体格式：[requestId(8B) | msgType(1B) | methodLen(2B) | method | payload]。
// 无线程亲和性，纯工具函数。

#include "mini/net/framing/PacketFramer.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mini::rpc {

/// Message types in the RPC protocol.
enum class RpcMsgType : std::uint8_t {
    kRequest = 0,
    kResponse = 1,
    kError = 2,
};

/// A decoded RPC frame.
struct RpcMessage {
    std::uint64_t requestId{0};
    RpcMsgType msgType{RpcMsgType::kRequest};
    std::string method;   // non-empty for request; empty for response/error
    std::string payload;  // opaque payload bytes
};

/// Result of a decode attempt.
enum class RpcDecodeResult {
    kComplete,    // A full frame was decoded
    kIncomplete,  // Need more data
    kError,       // Malformed frame
};

namespace codec {

/// Header sizes.
static constexpr std::size_t kHeaderLen = mini::net::framing::kFrameHeaderSize; // PacketFramer header size
static constexpr std::size_t kMinBodyLen = 8 + 1 + 2;  // requestId + msgType + methodLen

static constexpr std::uint32_t kRpcMsgId = 0x52504302;  // 'RPC\\x02'

/// Maximum frame body size (not including the PacketFramer header).
static constexpr std::size_t kMaxFrameBodySize = mini::net::framing::kDefaultMaxPayloadSize;

/// Encode a request frame.
/// @return The complete encoded frame bytes
std::string encodeRequest(std::uint64_t requestId,
                          std::string_view method,
                          std::string_view payload);

/// Encode a response frame.
std::string encodeResponse(std::uint64_t requestId,
                           std::string_view payload);

/// Encode an error frame.
std::string encodeError(std::uint64_t requestId,
                        std::string_view errorMessage);

/// Try to decode one RPC frame from raw bytes.
/// @param data   Pointer to the beginning of data
/// @param len    Number of available bytes
/// @param[out] msg      The decoded message (valid only if kComplete)
/// @param[out] consumed Number of bytes consumed (valid only if kComplete)
/// @return Decode result
RpcDecodeResult decode(const char* data, std::size_t len,
                       RpcMessage& msg, std::size_t& consumed);

/// Decode a fully buffered PacketFramer payload into an RPC message.
/// Caller should provide payload exactly as frame.payload (without packet header).
RpcDecodeResult decodePayload(std::string_view payload, RpcMessage& msg);

}  // namespace codec
}  // namespace mini::rpc
