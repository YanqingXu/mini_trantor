#include "mini/ws/WebSocketCodec.h"

#include <cstring>

namespace mini::ws::codec {

std::string encode(bool fin, WsOpcode opcode, std::string_view payload) {
    std::string frame;
    // Reserve approximate size: 2 header + possible extended length + payload
    frame.reserve(10 + payload.size());

    // Byte 0: FIN + opcode
    std::uint8_t byte0 = static_cast<std::uint8_t>(opcode);
    if (fin) byte0 |= 0x80;
    frame.push_back(static_cast<char>(byte0));

    // Byte 1: MASK=0 (server never masks) + payload length
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        frame.push_back(static_cast<char>(126));
        std::uint16_t len16 = static_cast<std::uint16_t>(payload.size());
        // Network byte order (big-endian)
        frame.push_back(static_cast<char>((len16 >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len16 & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        std::uint64_t len64 = payload.size();
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((len64 >> (i * 8)) & 0xFF));
        }
    }

    // Payload (no mask for server frames)
    frame.append(payload.data(), payload.size());
    return frame;
}

std::string encodeClose(WsCloseCode code, std::string_view reason) {
    std::string payload;
    auto codeVal = static_cast<std::uint16_t>(code);
    payload.push_back(static_cast<char>((codeVal >> 8) & 0xFF));
    payload.push_back(static_cast<char>(codeVal & 0xFF));
    payload.append(reason.data(), reason.size());
    return encode(true, WsOpcode::kClose, payload);
}

void unmask(char* data, std::size_t len, const std::uint8_t maskKey[4]) {
    for (std::size_t i = 0; i < len; ++i) {
        data[i] ^= static_cast<char>(maskKey[i % 4]);
    }
}

WsDecodeResult decode(const char* data, std::size_t len,
                      WsFrame& frame, std::size_t& consumed) {
    if (len < 2) return WsDecodeResult::kIncomplete;

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);

    // Byte 0
    frame.fin = (bytes[0] & 0x80) != 0;
    std::uint8_t rsv = (bytes[0] >> 4) & 0x07;
    if (rsv != 0) return WsDecodeResult::kError;  // No extensions supported

    frame.opcode = static_cast<WsOpcode>(bytes[0] & 0x0F);

    // Byte 1
    frame.masked = (bytes[1] & 0x80) != 0;
    std::uint8_t payloadLen7 = bytes[1] & 0x7F;

    std::size_t headerSize = 2;
    std::uint64_t payloadLength = 0;

    if (payloadLen7 < 126) {
        payloadLength = payloadLen7;
    } else if (payloadLen7 == 126) {
        headerSize += 2;
        if (len < headerSize) return WsDecodeResult::kIncomplete;
        payloadLength = (static_cast<std::uint64_t>(bytes[2]) << 8) |
                        static_cast<std::uint64_t>(bytes[3]);
    } else {  // 127
        headerSize += 8;
        if (len < headerSize) return WsDecodeResult::kIncomplete;
        payloadLength = 0;
        for (int i = 0; i < 8; ++i) {
            payloadLength = (payloadLength << 8) | bytes[2 + i];
        }
    }

    // Safety limit
    if (payloadLength > kMaxPayloadSize) return WsDecodeResult::kError;

    // Mask key (4 bytes if masked)
    std::uint8_t maskKey[4] = {};
    if (frame.masked) {
        headerSize += 4;
        if (len < headerSize) return WsDecodeResult::kIncomplete;
        std::memcpy(maskKey, data + headerSize - 4, 4);
    }

    // Check we have full payload
    std::size_t totalSize = headerSize + static_cast<std::size_t>(payloadLength);
    if (len < totalSize) return WsDecodeResult::kIncomplete;

    // Extract and unmask payload
    frame.payload.assign(data + headerSize, static_cast<std::size_t>(payloadLength));
    if (frame.masked) {
        unmask(frame.payload.data(), frame.payload.size(), maskKey);
    }

    // Parse close code if close frame
    if (frame.opcode == WsOpcode::kClose && frame.payload.size() >= 2) {
        auto* p = reinterpret_cast<const std::uint8_t*>(frame.payload.data());
        frame.closeCode = (static_cast<std::uint16_t>(p[0]) << 8) | p[1];
        frame.payload = frame.payload.substr(2);  // reason only
    }

    consumed = totalSize;
    return WsDecodeResult::kComplete;
}

}  // namespace mini::ws::codec
