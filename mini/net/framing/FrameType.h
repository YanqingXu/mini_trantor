#pragma once

// FrameType 定义 PacketFramer 的通用帧协议元信息。
// 采用网络字节序（大端）:
// magic(2) + payloadLen(4) + msgId(4) + flags(2) + seq(4) + payload(变长)
//
// 设计目标：
// - 统一处理粘包/半包；
// - 可在不同消息协议中复用（只复用上层 msgId/flags/seq 语义）；
// - 同一连接内仅在 owner loop 的解码链路上使用。

#include <cstddef>
#include <cstdint>

namespace mini::net::framing {

// 默认魔数，表征统一帧格式版本。
inline constexpr std::uint16_t kDefaultPacketMagic = 0x5046;  // 'P''F'

// 帧头固定长度（不含 payload）。
inline constexpr std::size_t kFrameHeaderSize = 2 + 4 + 4 + 2 + 4;

// 默认最大 payload，超过该值将返回 over-limit。
inline constexpr std::size_t kDefaultMaxPayloadSize = 64 * 1024;

struct PacketHeader {
    std::uint16_t magic{kDefaultPacketMagic};
    std::uint32_t payloadLen{0};
    std::uint32_t msgId{0};
    std::uint16_t flags{0};
    std::uint32_t seq{0};
};

enum class PacketDecodeState { kComplete, kNeedMore, kInvalid, kOverLimit };

using PacketFlags = std::uint16_t;

inline constexpr PacketFlags kPacketFlagNone = 0x0000;

}  // namespace mini::net::framing
