// Unit tests for PacketFramer.
// Cover: encode/decode、分片/粘包边界、非法魔数、超限防护、跨帧复用解析。

#include "mini/net/framing/PacketFramer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <array>
#include <string>
#include <string_view>

int main() {
    using namespace mini::net::framing;

    const auto writeHeader = [](std::uint16_t magic,
                               std::uint32_t payloadLen,
                               std::uint32_t msgId,
                               std::uint16_t flags,
                               std::uint32_t seq) {
        std::string frame;
        frame.push_back(static_cast<char>((magic >> 8) & 0xFF));
        frame.push_back(static_cast<char>(magic & 0xFF));
        frame.push_back(static_cast<char>((payloadLen >> 24) & 0xFF));
        frame.push_back(static_cast<char>((payloadLen >> 16) & 0xFF));
        frame.push_back(static_cast<char>((payloadLen >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payloadLen & 0xFF));
        frame.push_back(static_cast<char>((msgId >> 24) & 0xFF));
        frame.push_back(static_cast<char>((msgId >> 16) & 0xFF));
        frame.push_back(static_cast<char>((msgId >> 8) & 0xFF));
        frame.push_back(static_cast<char>(msgId & 0xFF));
        frame.push_back(static_cast<char>((flags >> 8) & 0xFF));
        frame.push_back(static_cast<char>(flags & 0xFF));
        frame.push_back(static_cast<char>((seq >> 24) & 0xFF));
        frame.push_back(static_cast<char>((seq >> 16) & 0xFF));
        frame.push_back(static_cast<char>((seq >> 8) & 0xFF));
        frame.push_back(static_cast<char>(seq & 0xFF));
        return frame;
    };

    PacketFramer framer;

    // 1. Encode + decode round trip
    {
        std::string payload = "hello world";
        std::string frame = framer.encode(0x12345678, 0x0007, 0x9ABCDEF0, payload);
        Packet packet;
        std::size_t consumed = 0;
        auto state = framer.decode(frame.data(), frame.size(), packet, consumed);
        assert(state == PacketDecodeState::kComplete);
        assert(consumed == frame.size());
        assert(packet.header.magic == kDefaultPacketMagic);
        assert(packet.header.msgId == 0x12345678);
        assert(packet.header.flags == 0x0007);
        assert(packet.header.seq == 0x9ABCDEF0);
        assert(packet.payload == payload);
        std::printf("  PASS: encode/decode round trip\n");
    }

    // 2. Header incomplete
    {
        std::string payload = "x";
        std::string frame = framer.encode(1, 0, 0, payload);
        Packet packet;
        std::size_t consumed = 0;
        auto state = framer.decode(frame.data(), kFrameHeaderSize - 1, packet, consumed);
        assert(state == PacketDecodeState::kNeedMore);
        assert(consumed == 0);
        std::printf("  PASS: incomplete header\n");
    }

    // 3. Payload incomplete
    {
        std::string payload = "payload-data";
        std::string frame = framer.encode(1, 0, 0, payload);
        Packet packet;
        std::size_t consumed = 0;
        auto state = framer.decode(frame.data(), frame.size() - 1, packet, consumed);
        assert(state == PacketDecodeState::kNeedMore);
        assert(consumed == 0);
        std::printf("  PASS: incomplete payload\n");
    }

    // 4. Invalid magic number
    {
        std::string payload = "bad";
        std::string frame = framer.encode(1, 0, 0, payload);
        frame[0] = static_cast<char>(frame[0] ^ 0xFF);
        Packet packet;
        std::size_t consumed = 0;
        auto state = framer.decode(frame.data(), frame.size(), packet, consumed);
        assert(state == PacketDecodeState::kInvalid);
        std::printf("  PASS: invalid magic\n");
    }

    // 5. Multiple frames in one buffer
    {
        std::string frame1 = framer.encode(11, 0, 1, std::string_view("first"));
        std::string frame2 = framer.encode(22, 0, 2, std::string_view("second"));
        std::string combined = frame1 + frame2;
        Packet packet;
        std::size_t consumed = 0;
        auto state1 = framer.decode(combined.data(), combined.size(), packet, consumed);
        assert(state1 == PacketDecodeState::kComplete);
        assert(packet.payload == "first");
        Packet packet2;
        std::size_t consumed2 = 0;
        auto state2 = framer.decode(combined.data() + consumed, combined.size() - consumed,
                                    packet2, consumed2);
        assert(state2 == PacketDecodeState::kComplete);
        assert(packet2.payload == "second");
        std::printf("  PASS: multiple frame decode\n");
    }

    // 6. Over-limit payload
    {
        PacketFramer tinyFramer(kDefaultPacketMagic, 4);
        auto header = writeHeader(kDefaultPacketMagic, 8, 0x01, 0, 0);
        Packet packet;
        std::size_t consumed = 0;
        auto state = tinyFramer.decode(header.data(), header.size(), packet, consumed);
        assert(state == PacketDecodeState::kOverLimit);
        assert(consumed == 0);
        std::printf("  PASS: over-limit payload\n");
    }

    // 7. Batch decode with frame budget
    {
        std::array<Packet, 2> packets{};
        std::string frame1 = framer.encode(11, 0, 1, std::string_view("a"));
        std::string frame2 = framer.encode(22, 0, 2, std::string_view("b"));
        std::string frame3 = framer.encode(33, 0, 3, std::string_view("c"));
        std::string stream = frame1 + frame2 + frame3;

        auto first = framer.decodeBatch(stream.data(), stream.size(),
                                       packets.data(), packets.size(), 2);
        assert(first.status == PacketDecodeState::kNeedMore);
        assert(first.hitLimit);
        assert(first.frameCount == 2);
        assert(first.consumed == frame1.size() + frame2.size());
        assert(packets[0].payload == "a");
        assert(packets[1].payload == "b");

        std::array<Packet, 2> tailPackets{};
        auto second = framer.decodeBatch(stream.data() + first.consumed,
                                        stream.size() - first.consumed,
                                        tailPackets.data(), tailPackets.size(), 2);
        assert(second.status == PacketDecodeState::kComplete);
        assert(second.frameCount == 1);
        assert(second.consumed == frame3.size());
        assert(tailPackets[0].payload == "c");
        std::printf("  PASS: batch decode limit and resume\n");
    }

    std::printf("All PacketFramer unit tests passed.\n");
    return 0;
}
