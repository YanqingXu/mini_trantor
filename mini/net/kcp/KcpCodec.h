#pragma once

// KcpCodec — 轻量 KCP 风格帧编解码工具。
//
// 帧头采用“有界固定头 + payload”的方式封装会话上下文与可靠性语义：
// magic/version/flags/sessionId/seq/ack/payloadLen/payload。
//
// 说明：
// - sessionId/seq/ack 使用网络字节序传输；
// - payload 长度由 payloadLen 描述；
// - 对于仅 ACK 帧可发送 payloadLen=0、flags 包含 kFlagAck；
// - 该编码器不参与重传决策，仅负责幂等可复用的帧格式转换。

#include "mini/net/transport/TransportTypes.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace mini::net::kcp {
namespace codec {

// 固定帧头长度（不含 payload）
// magic(2) + version(1) + flags(1) + sessionId(8) + seq(4) + ack(4) + payloadLen(2)
inline constexpr std::size_t kKcpFrameHeaderSize = 22;
inline constexpr std::uint16_t kKcpFrameMagic = 0x4b43;  // 'K','C'
inline constexpr std::uint8_t kKcpFrameVersion = 1;
inline constexpr std::size_t kKcpMaxPayloadSize = 64 * 1024;

enum KcpFrameFlags : std::uint16_t {
    kKcpFrameFlagNone = 0x0000,
    kKcpFrameFlagData = 0x0001,
    kKcpFrameFlagAck = 0x0002,
    kKcpFrameFlagReset = 0x0004,
};

// 已解码的 KCP 帧
struct KcpFrame {
    transport::TransportSessionId sessionId{transport::kInvalidTransportSessionId};
    std::uint32_t seq{0};
    std::uint32_t ack{0};
    std::uint16_t flags{kKcpFrameFlagNone};
    std::string payload;
};

// 将帧编码为可直接作为 UDP payload 发送的字节串。
std::string encodeFrame(const KcpFrame& frame);

// 解析 UDP payload，返回 true 表示解码成功。
bool decodeFrame(std::string_view data, KcpFrame& out);

}  // namespace codec
}  // namespace mini::net::kcp
