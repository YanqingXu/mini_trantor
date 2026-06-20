#pragma once

// PacketFramer：统一的连接帧包装/解码模块。
// 负责：
// - 帧结构构造（固定头 + payloadLen 可变长度体）
// - 完整/不完整/非法/超限状态判断
//
// 该模块只处理字节级封包，不包含业务语义；业务字段通过 msgId/flags/seq 注入。

#include "mini/net/framing/FrameType.h"

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <string>
#include <string_view>

namespace mini::net::framing {

// 绑定 payload 视图的解码结果。payload 指向输入缓冲区，消费后立即读取即可。
struct Packet {
    PacketHeader header{};
    std::string_view payload{};
};

struct PacketBatchResult {
    PacketDecodeState status{PacketDecodeState::kNeedMore};
    std::size_t consumed{0};
    std::size_t frameCount{0};
    bool hitLimit{false};
};

inline constexpr std::size_t kDefaultMaxFramesPerBatch = 16;

std::string encodeFrame(const PacketHeader& header,
                       std::string_view payload,
                       std::size_t maxPayload = kDefaultMaxPayloadSize);

PacketDecodeState decodeFrame(const char* data,
                             std::size_t len,
                             Packet& packet,
                             std::size_t& consumed,
                             std::size_t maxPayload = kDefaultMaxPayloadSize,
                             std::uint16_t expectedMagic = kDefaultPacketMagic);

// 预绑定参数的轻量封装，便于上层模块在循环内复用一致策略。
class PacketFramer {
public:
    PacketFramer(std::uint16_t expectedMagic = kDefaultPacketMagic,
                 std::size_t maxPayload = kDefaultMaxPayloadSize) noexcept
        : expectedMagic_(expectedMagic),
          maxPayload_(maxPayload) {
    }

    std::string encode(std::uint32_t msgId,
                       std::uint16_t flags,
                       std::uint32_t seq,
                       std::string_view payload) const {
        PacketHeader header{expectedMagic_, 0, msgId, flags, seq};
        return encodeFrame(header, payload, maxPayload_);
    }

    PacketDecodeState decode(const char* data,
                            std::size_t len,
                            Packet& packet,
                            std::size_t& consumed) const noexcept {
        return decodeFrame(data, len, packet, consumed, maxPayload_, expectedMagic_);
    }

    PacketBatchResult decodeBatch(const char* data,
                                 std::size_t len,
                                 Packet* packets,
                                 std::size_t maxPackets,
                                 std::size_t maxFramesPerBatch = kDefaultMaxFramesPerBatch) const noexcept {
        PacketBatchResult result{};
        result.status = PacketDecodeState::kNeedMore;

        if (maxPackets == 0) {
            result.hitLimit = (len > 0);
            if (len == 0) {
                result.status = PacketDecodeState::kComplete;
            }
            return result;
        }

        const std::size_t frameBudget = (maxFramesPerBatch == 0)
                                           ? maxPackets
                                           : std::min(maxPackets, maxFramesPerBatch);

        std::size_t offset = 0;
        while (offset < len && result.frameCount < frameBudget) {
            Packet packet;
            std::size_t consumed = 0;
            const auto state = decodeFrame(data + offset, len - offset, packet, consumed,
                                          maxPayload_, expectedMagic_);
            if (state != PacketDecodeState::kComplete) {
                result.status = state;
                result.consumed = offset;
                return result;
            }

            if (packets) {
                packets[result.frameCount] = packet;
            }
            ++result.frameCount;
            offset += consumed;
        }

        result.consumed = offset;
        if (offset < len) {
            result.status = PacketDecodeState::kNeedMore;
            result.hitLimit = (result.frameCount == frameBudget);
        } else {
            result.status = PacketDecodeState::kComplete;
        }
        return result;
    }

private:
    std::uint16_t expectedMagic_;
    std::size_t maxPayload_;
};

}  // namespace mini::net::framing
