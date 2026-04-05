// Unit tests for WebSocket frame encoding and decoding.
// Pure stateless codec tests — no EventLoop or networking needed.

#include "mini/ws/WebSocketCodec.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

using namespace mini::ws;
using namespace std::string_view_literals;

int main() {
    // 1. Encode text frame — small payload (< 126 bytes)
    {
        std::string frame = codec::encodeText("hello");
        assert(frame.size() == 2 + 5);  // 2 header + 5 payload
        auto* b = reinterpret_cast<const std::uint8_t*>(frame.data());
        assert(b[0] == 0x81);  // FIN=1, opcode=text
        assert(b[1] == 5);     // MASK=0, len=5
        assert(frame.substr(2) == "hello");
        std::printf("  PASS: encode text frame (small)\n");
    }

    // 2. Encode binary frame
    {
        std::string frame = codec::encodeBinary("\x00\x01\x02"sv);
        auto* b = reinterpret_cast<const std::uint8_t*>(frame.data());
        assert(b[0] == 0x82);  // FIN=1, opcode=binary
        assert(b[1] == 3);
        std::printf("  PASS: encode binary frame\n");
    }

    // 3. Encode ping frame
    {
        std::string frame = codec::encodePing("ping");
        auto* b = reinterpret_cast<const std::uint8_t*>(frame.data());
        assert(b[0] == 0x89);  // FIN=1, opcode=ping
        assert(b[1] == 4);
        assert(frame.substr(2) == "ping");
        std::printf("  PASS: encode ping frame\n");
    }

    // 4. Encode pong frame
    {
        std::string frame = codec::encodePong("pong");
        auto* b = reinterpret_cast<const std::uint8_t*>(frame.data());
        assert(b[0] == 0x8A);  // FIN=1, opcode=pong
        std::printf("  PASS: encode pong frame\n");
    }

    // 5. Encode close frame with code and reason
    {
        std::string frame = codec::encodeClose(WsCloseCode::kNormal, "bye");
        auto* b = reinterpret_cast<const std::uint8_t*>(frame.data());
        assert(b[0] == 0x88);  // FIN=1, opcode=close
        assert(b[1] == 5);     // 2-byte code + 3-byte reason
        assert(b[2] == 0x03);  // 1000 >> 8
        assert(b[3] == 0xE8);  // 1000 & 0xFF
        assert(frame.substr(4) == "bye");
        std::printf("  PASS: encode close frame\n");
    }

    // 6. Encode 16-bit extended length (payload = 200 bytes)
    {
        std::string payload(200, 'x');
        std::string frame = codec::encodeText(payload);
        auto* b = reinterpret_cast<const std::uint8_t*>(frame.data());
        assert(b[0] == 0x81);
        assert(b[1] == 126);  // Extended 16-bit length
        std::uint16_t len16 = (static_cast<std::uint16_t>(b[2]) << 8) | b[3];
        assert(len16 == 200);
        assert(frame.size() == 4 + 200);
        std::printf("  PASS: encode 16-bit extended length\n");
    }

    // 7. Decode unmasked server frame
    {
        std::string encoded = codec::encodeText("world");
        WsFrame frame;
        std::size_t consumed = 0;
        auto result = codec::decode(encoded.data(), encoded.size(), frame, consumed);
        assert(result == WsDecodeResult::kComplete);
        assert(frame.fin);
        assert(frame.opcode == WsOpcode::kText);
        assert(!frame.masked);
        assert(frame.payload == "world");
        assert(consumed == encoded.size());
        std::printf("  PASS: decode unmasked frame\n");
    }

    // 8. Decode masked client frame
    {
        // Manually construct a masked text frame: "Hi"
        std::string raw;
        raw.push_back(static_cast<char>(0x81));   // FIN=1, text
        raw.push_back(static_cast<char>(0x82));   // MASK=1, len=2
        // Mask key
        std::uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
        raw.append(reinterpret_cast<const char*>(mask), 4);
        // Masked payload
        raw.push_back(static_cast<char>('H' ^ 0x12));
        raw.push_back(static_cast<char>('i' ^ 0x34));

        WsFrame frame;
        std::size_t consumed = 0;
        auto result = codec::decode(raw.data(), raw.size(), frame, consumed);
        assert(result == WsDecodeResult::kComplete);
        assert(frame.masked);
        assert(frame.payload == "Hi");
        assert(consumed == raw.size());
        std::printf("  PASS: decode masked client frame\n");
    }

    // 9. Decode incomplete frame
    {
        std::string partial;
        partial.push_back(static_cast<char>(0x81));  // Just first byte
        WsFrame frame;
        std::size_t consumed = 0;
        auto result = codec::decode(partial.data(), partial.size(), frame, consumed);
        assert(result == WsDecodeResult::kIncomplete);
        std::printf("  PASS: decode incomplete frame\n");
    }

    // 10. Decode incomplete payload
    {
        std::string raw;
        raw.push_back(static_cast<char>(0x81));  // FIN=1, text
        raw.push_back(static_cast<char>(10));    // len=10 but we only provide 3
        raw.append("abc");
        WsFrame frame;
        std::size_t consumed = 0;
        auto result = codec::decode(raw.data(), raw.size(), frame, consumed);
        assert(result == WsDecodeResult::kIncomplete);
        std::printf("  PASS: decode incomplete payload\n");
    }

    // 11. Decode close frame with code
    {
        std::string encoded = codec::encodeClose(WsCloseCode::kGoingAway, "shutdown");
        WsFrame frame;
        std::size_t consumed = 0;
        auto result = codec::decode(encoded.data(), encoded.size(), frame, consumed);
        assert(result == WsDecodeResult::kComplete);
        assert(frame.opcode == WsOpcode::kClose);
        assert(frame.closeCode == 1001);
        assert(frame.payload == "shutdown");
        std::printf("  PASS: decode close frame with code\n");
    }

    // 12. Decode rejects RSV bits set
    {
        std::string raw;
        raw.push_back(static_cast<char>(0xC1));  // FIN=1, RSV1=1, text
        raw.push_back(static_cast<char>(0));
        WsFrame frame;
        std::size_t consumed = 0;
        auto result = codec::decode(raw.data(), raw.size(), frame, consumed);
        assert(result == WsDecodeResult::kError);
        std::printf("  PASS: decode rejects RSV bits\n");
    }

    // 13. unmask utility
    {
        char data[] = "abcd";
        std::uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
        codec::unmask(data, 4, mask);
        assert(data[0] == ('a' ^ 0x11));
        assert(data[1] == ('b' ^ 0x22));
        assert(data[2] == ('c' ^ 0x33));
        assert(data[3] == ('d' ^ 0x44));
        // Unmask again to get original
        codec::unmask(data, 4, mask);
        assert(std::string(data, 4) == "abcd");
        std::printf("  PASS: unmask utility\n");
    }

    std::printf("All WebSocket codec unit tests passed.\n");
    return 0;
}
