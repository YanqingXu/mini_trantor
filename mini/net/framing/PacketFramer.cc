#include "mini/net/framing/PacketFramer.h"

#include <cstdint>

namespace mini::net::framing {

namespace {

void appendUint16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

void appendUint32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

std::uint16_t readUint16(const char* data) {
    auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
    return (static_cast<std::uint16_t>(bytes[0]) << 8) |
           static_cast<std::uint16_t>(bytes[1]);
}

std::uint32_t readUint32(const char* data) {
    auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

PacketHeader parseHeader(const char* data) {
    PacketHeader header;
    header.magic = readUint16(data);
    header.payloadLen = readUint32(data + 2);
    header.msgId = readUint32(data + 6);
    header.flags = readUint16(data + 10);
    header.seq = readUint32(data + 12);
    return header;
}

}  // namespace

std::string encodeFrame(const PacketHeader& header,
                       std::string_view payload,
                       std::size_t maxPayload) {
    if (payload.size() > maxPayload) {
        return {};
    }

    PacketHeader normalized = header;
    normalized.payloadLen = static_cast<std::uint32_t>(payload.size());

    std::string frame;
    frame.reserve(kFrameHeaderSize + payload.size());
    appendUint16(frame, normalized.magic);
    appendUint32(frame, normalized.payloadLen);
    appendUint32(frame, normalized.msgId);
    appendUint16(frame, normalized.flags);
    appendUint32(frame, normalized.seq);
    frame.append(payload.data(), payload.size());
    return frame;
}

PacketDecodeState decodeFrame(const char* data,
                             std::size_t len,
                             Packet& packet,
                             std::size_t& consumed,
                             std::size_t maxPayload,
                             std::uint16_t expectedMagic) {
    consumed = 0;
    if (len < kFrameHeaderSize) {
        return PacketDecodeState::kNeedMore;
    }

    PacketHeader header = parseHeader(data);
    if (header.magic != expectedMagic) {
        return PacketDecodeState::kInvalid;
    }

    if (header.payloadLen > maxPayload) {
        return PacketDecodeState::kOverLimit;
    }

    const std::size_t totalLen = kFrameHeaderSize + static_cast<std::size_t>(header.payloadLen);
    if (len < totalLen) {
        return PacketDecodeState::kNeedMore;
    }

    packet.header = header;
    packet.payload = std::string_view(data + kFrameHeaderSize, header.payloadLen);
    consumed = totalLen;
    return PacketDecodeState::kComplete;
}

}  // namespace mini::net::framing
