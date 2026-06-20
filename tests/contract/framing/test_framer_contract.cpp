// Contract tests for PacketFramer as transport-agnostic framing primitive.
// Ensure over-limit returns are deterministic and partial data can be resumed.

#include "mini/net/framing/PacketFramer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <array>
#include <string>

int main() {
    using namespace mini::net::framing;

    PacketFramer framer;
    Packet packet;

    // Contract 1: Incomplete data is always reported explicitly.
    {
        std::string tinyPayload = framer.encode(7, 0, 3, "ping");
        std::size_t consumed = 0;
        auto state = framer.decode(tinyPayload.data(),
                                   tinyPayload.size() - 2,
                                   packet,
                                   consumed);
        assert(state == PacketDecodeState::kNeedMore);
        assert(consumed == 0);
        assert(packet.payload.empty());
        std::printf("  PASS: incomplete payload is explicitly exposed\n");
    }

    // Contract 2: Over-limit is detected before payload read is required.
    {
        PacketFramer tinyFramer(kDefaultPacketMagic, 1);
        std::string header;
        header.push_back(static_cast<char>(kDefaultPacketMagic >> 8));
        header.push_back(static_cast<char>(kDefaultPacketMagic & 0xFF));
        header.push_back(0);
        header.push_back(0);
        header.push_back(0);
        header.push_back(2);
        header.append(std::string(kFrameHeaderSize - 6, '\0'));  // total header length is 16

        std::size_t consumed = 0;
        auto state = tinyFramer.decode(header.data(), header.size(), packet, consumed);
        assert(state == PacketDecodeState::kOverLimit);
        assert(consumed == 0);
        std::printf("  PASS: over-limit payload is rejected without reading payload body\n");
    }

    // Contract 3: Decoder can continue when more bytes arrive.
    {
        std::string full = framer.encode(99, 0, 7, "contract");
        const std::size_t split = framer.encode(99, 0, 7, "").size();
        std::size_t consumed = 0;
        auto state1 = framer.decode(full.data(), split - 1, packet, consumed);
        assert(state1 == PacketDecodeState::kNeedMore);

        auto state2 = framer.decode(full.data(), full.size(), packet, consumed);
        assert(state2 == PacketDecodeState::kComplete);
        assert(packet.payload == "contract");
        assert(consumed == full.size());
        std::printf("  PASS: incomplete chunk can continue in later reads\n");
    }

    // Contract 4: Batch decode consumes at most one batch quota and can be resumed safely.
    {
        std::string stream;
        stream.reserve(3 * 16);
        std::string frame1 = framer.encode(1, 0, 1, "first");
        std::string frame2 = framer.encode(2, 0, 1, "second");
        std::string frame3 = framer.encode(3, 0, 1, "third");
        stream += frame1;
        stream += frame2;
        stream += frame3;

        std::array<Packet, 2> packets{};
        auto first = framer.decodeBatch(stream.data(), stream.size(), packets.data(), packets.size(), 2);
        assert(first.status == PacketDecodeState::kNeedMore);
        assert(first.hitLimit);
        assert(first.frameCount == 2);
        assert(first.consumed == frame1.size() + frame2.size());

        std::array<Packet, 1> rest{};
        auto second = framer.decodeBatch(stream.data() + first.consumed,
                                        stream.size() - first.consumed,
                                        rest.data(), rest.size(), 2);
        assert(second.status == PacketDecodeState::kComplete);
        assert(second.frameCount == 1);
        assert(second.consumed == frame3.size());
        assert(rest[0].payload == "third");
    }

    std::printf("All PacketFramer contract tests passed.\n");
    return 0;
}
