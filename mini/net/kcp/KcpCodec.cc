#include "mini/net/kcp/KcpCodec.h"

#include "mini/net/SocketTypes.h"

#include <cstring>

namespace mini::net::kcp {
namespace codec {

namespace {

void appendUint16(std::string& out, std::uint16_t value) {
    auto net = htons(value);
    out.append(reinterpret_cast<const char*>(&net), sizeof(net));
}

void appendUint32(std::string& out, std::uint32_t value) {
    auto net = htonl(value);
    out.append(reinterpret_cast<const char*>(&net), sizeof(net));
}

void appendUint64(std::string& out, std::uint64_t value) {
    auto high = htonl(static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFULL));
    auto low = htonl(static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
    out.append(reinterpret_cast<const char*>(&high), sizeof(high));
    out.append(reinterpret_cast<const char*>(&low), sizeof(low));
}

std::uint16_t readUint16(const char* data) {
    std::uint16_t net = 0;
    std::memcpy(&net, data, sizeof(net));
    return ntohs(net);
}

std::uint32_t readUint32(const char* data) {
    std::uint32_t net = 0;
    std::memcpy(&net, data, sizeof(net));
    return ntohl(net);
}

std::uint64_t readUint64(const char* data) {
    std::uint32_t high = 0;
    std::uint32_t low = 0;
    std::memcpy(&high, data, sizeof(high));
    std::memcpy(&low, data + sizeof(high), sizeof(low));
    auto h = ntohl(high);
    auto l = ntohl(low);
    return (static_cast<std::uint64_t>(h) << 32) | l;
}

}  // namespace

std::string encodeFrame(const KcpFrame& frame) {
    if (frame.payload.size() > kKcpMaxPayloadSize) {
        return {};
    }

    std::string out;
    out.reserve(kKcpFrameHeaderSize + frame.payload.size());
    appendUint16(out, kKcpFrameMagic);
    out.push_back(static_cast<char>(kKcpFrameVersion));
    out.push_back(static_cast<char>(frame.flags));
    appendUint64(out, frame.sessionId);
    appendUint32(out, frame.seq);
    appendUint32(out, frame.ack);
    appendUint16(out, static_cast<std::uint16_t>(frame.payload.size()));
    out.append(frame.payload);
    return out;
}

bool decodeFrame(std::string_view data, KcpFrame& out) {
    if (data.size() < kKcpFrameHeaderSize) {
        return false;
    }

    const auto* raw = data.data();
    const std::uint16_t magic = readUint16(raw);
    if (magic != kKcpFrameMagic) {
        return false;
    }

    const auto version = static_cast<std::uint8_t>(raw[2]);
    if (version != kKcpFrameVersion) {
        return false;
    }

    out.sessionId = readUint64(raw + 4);
    out.seq = readUint32(raw + 12);
    out.ack = readUint32(raw + 16);
    out.flags = static_cast<std::uint16_t>(static_cast<std::uint8_t>(raw[3]));
    const std::uint16_t payloadLen = readUint16(raw + 20);

    if (payloadLen > kKcpMaxPayloadSize ||
        static_cast<std::size_t>(payloadLen) + kKcpFrameHeaderSize != data.size()) {
        return false;
    }

    out.payload.assign(raw + kKcpFrameHeaderSize, payloadLen);
    return true;
}

}  // namespace codec
}  // namespace mini::net::kcp
