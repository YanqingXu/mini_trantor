// Unit tests for RpcCodec — encode/decode round-trip, partial frames, error cases.
// Pure utility tests — no EventLoop or networking needed.

#include "mini/rpc/RpcCodec.h"
#include "mini/net/framing/PacketFramer.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>

using namespace mini::rpc;
using namespace mini::rpc::codec;
using namespace mini::net::framing;

int main() {
    const auto writeUint16 = [](std::string& dst, std::uint16_t value) {
        dst.push_back(static_cast<char>((value >> 8) & 0xFF));
        dst.push_back(static_cast<char>(value & 0xFF));
    };

    const auto writeUint32 = [](std::string& dst, std::uint32_t value) {
        dst.push_back(static_cast<char>((value >> 24) & 0xFF));
        dst.push_back(static_cast<char>((value >> 16) & 0xFF));
        dst.push_back(static_cast<char>((value >> 8) & 0xFF));
        dst.push_back(static_cast<char>(value & 0xFF));
    };

    const auto makeEmptyBodyHeader = [&](std::uint32_t payloadLen,
                                        std::uint32_t msgId = 0x52504302) {
        std::string frame;
        writeUint16(frame, kDefaultPacketMagic);
        writeUint32(frame, payloadLen);
        writeUint32(frame, msgId);
        writeUint16(frame, kPacketFlagNone);
        writeUint32(frame, 0);
        return frame;
    };

    // 1. Encode and decode a request frame
    {
        std::string frame = encodeRequest(42, "Echo.Say", "hello");
        RpcMessage msg;
        std::size_t consumed = 0;
        auto result = decode(frame.data(), frame.size(), msg, consumed);

        assert(result == RpcDecodeResult::kComplete);
        assert(consumed == frame.size());
        assert(msg.requestId == 42);
        assert(msg.msgType == RpcMsgType::kRequest);
        assert(msg.method == "Echo.Say");
        assert(msg.payload == "hello");
        std::printf("  PASS: encode/decode request round-trip\n");
    }

    // 2. Encode and decode a response frame
    {
        std::string frame = encodeResponse(99, "world");
        RpcMessage msg;
        std::size_t consumed = 0;
        auto result = decode(frame.data(), frame.size(), msg, consumed);

        assert(result == RpcDecodeResult::kComplete);
        assert(consumed == frame.size());
        assert(msg.requestId == 99);
        assert(msg.msgType == RpcMsgType::kResponse);
        assert(msg.method.empty());
        assert(msg.payload == "world");
        std::printf("  PASS: encode/decode response round-trip\n");
    }

    // 3. Encode and decode an error frame
    {
        std::string frame = encodeError(7, "method not found");
        RpcMessage msg;
        std::size_t consumed = 0;
        auto result = decode(frame.data(), frame.size(), msg, consumed);

        assert(result == RpcDecodeResult::kComplete);
        assert(msg.requestId == 7);
        assert(msg.msgType == RpcMsgType::kError);
        assert(msg.payload == "method not found");
        std::printf("  PASS: encode/decode error round-trip\n");
    }

    // 4. Empty payload
    {
        std::string frame = encodeRequest(1, "Ping", "");
        RpcMessage msg;
        std::size_t consumed = 0;
        auto result = decode(frame.data(), frame.size(), msg, consumed);

        assert(result == RpcDecodeResult::kComplete);
        assert(msg.method == "Ping");
        assert(msg.payload.empty());
        std::printf("  PASS: empty payload\n");
    }

    // 5. Incomplete frame — too short for header
    {
        RpcMessage msg;
        std::size_t consumed = 0;
        auto result = decode("ab", 2, msg, consumed);
        assert(result == RpcDecodeResult::kIncomplete);
        std::printf("  PASS: incomplete — too short for header\n");
    }

    // 6. Incomplete frame — header present but body not fully arrived
    {
        std::string frame = encodeRequest(1, "Test", "some data");
        // Truncate: give only half the frame
        std::size_t half = frame.size() / 2;
        RpcMessage msg;
        std::size_t consumed = 0;
        auto result = decode(frame.data(), half, msg, consumed);
        assert(result == RpcDecodeResult::kIncomplete);
        std::printf("  PASS: incomplete — partial body\n");
    }

    // 7. Two frames concatenated — decode first, then second
    {
        std::string frame1 = encodeRequest(10, "A", "first");
        std::string frame2 = encodeResponse(20, "second");
        std::string combined = frame1 + frame2;

        RpcMessage msg1;
        std::size_t consumed1 = 0;
        auto r1 = decode(combined.data(), combined.size(), msg1, consumed1);
        assert(r1 == RpcDecodeResult::kComplete);
        assert(msg1.requestId == 10);
        assert(msg1.method == "A");

        RpcMessage msg2;
        std::size_t consumed2 = 0;
        auto r2 = decode(combined.data() + consumed1,
                         combined.size() - consumed1, msg2, consumed2);
        assert(r2 == RpcDecodeResult::kComplete);
        assert(msg2.requestId == 20);
        assert(msg2.payload == "second");
        std::printf("  PASS: two concatenated frames\n");
    }

    // 8. Invalid message type
    {
        std::string frame = encodeRequest(1, "X", "data");
        // msgType is at offset packetHeader(16) + 8 = 24
        frame[24] = static_cast<char>(0xFF);
        RpcMessage msg;
        std::size_t consumed = 0;
        auto result = decode(frame.data(), frame.size(), msg, consumed);
        assert(result == RpcDecodeResult::kError);
        std::printf("  PASS: invalid message type\n");
    }

    // 9. Frame body too large (exceeds max)
    {
        // Manually craft a packet header with payloadLen > kMaxFrameBodySize.
        std::string frame = makeEmptyBodyHeader(static_cast<std::uint32_t>(kMaxFrameBodySize + 1));
        frame.resize(frame.size() + 8, '\0');

        RpcMessage msg;
        std::size_t consumed = 0;
        auto result = decode(frame.data(), frame.size(), msg, consumed);
        assert(result == RpcDecodeResult::kError);
        std::printf("  PASS: frame body too large\n");
    }

    // 10. Body too short (less than minimum body length)
    {
        std::string frame = makeEmptyBodyHeader(5);
        frame.resize(frame.size() + 5, '\0');

        RpcMessage msg;
        std::size_t consumed = 0;
        auto result = decode(frame.data(), frame.size(), msg, consumed);
        assert(result == RpcDecodeResult::kError);
        std::printf("  PASS: body too short\n");
    }

    // 11. methodLen exceeds remaining body
    {
        std::string frame = encodeRequest(1, "X", "");
        // methodLen is at offset packetHeader(16) + 9 = 25 (2 bytes, big-endian)
        // Set it to a huge value
        frame[25] = static_cast<char>(0xFF);
        frame[26] = static_cast<char>(0xFF);
        RpcMessage msg;
        std::size_t consumed = 0;
        auto result = decode(frame.data(), frame.size(), msg, consumed);
        assert(result == RpcDecodeResult::kError);
        std::printf("  PASS: methodLen exceeds body\n");
    }

    // 12. Large payload round-trip
    {
        std::string bigPayload(60000, 'Z');
        std::string frame = encodeRequest(123, "BigCall", bigPayload);
        RpcMessage msg;
        std::size_t consumed = 0;
        auto result = decode(frame.data(), frame.size(), msg, consumed);
        assert(result == RpcDecodeResult::kComplete);
        assert(msg.payload == bigPayload);
        assert(msg.method == "BigCall");
        std::printf("  PASS: large payload round-trip\n");
    }

    std::printf("All RpcCodec unit tests passed.\n");
    return 0;
}
