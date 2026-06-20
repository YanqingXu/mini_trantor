// Integration test: Protobuf-style codec + PacketFramer framing pipeline.
// Verifies serialized message can pass transport framing and be decoded by another codec.

#include "mini/codec/ProtobufAdapter.h"
#include "mini/net/framing/PacketFramer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

struct GameMessage {
    std::uint32_t version{0};
    std::string body;

    bool SerializeToString(std::string* out) const {
        out->clear();
        out->reserve(4 + body.size());
        out->push_back(static_cast<char>((version >> 24) & 0xFF));
        out->push_back(static_cast<char>((version >> 16) & 0xFF));
        out->push_back(static_cast<char>((version >> 8) & 0xFF));
        out->push_back(static_cast<char>(version & 0xFF));
        out->append(body);
        return true;
    }

    bool ParseFromArray(const void* data, int len) {
        if (len < 4) {
            return false;
        }
        auto* p = static_cast<const unsigned char*>(data);
        version = (static_cast<std::uint32_t>(p[0]) << 24) |
                  (static_cast<std::uint32_t>(p[1]) << 16) |
                  (static_cast<std::uint32_t>(p[2]) << 8) |
                  static_cast<std::uint32_t>(p[3]);
        body.assign(static_cast<const char*>(data) + 4, static_cast<std::size_t>(len - 4));
        return true;
    }
};

}  // namespace

int main() {
    using namespace mini::codec;
    using namespace mini::net::framing;

    static constexpr std::uint32_t kGameMsgId = 0x47414d50;  // 'GAMP'
    PacketFramer framer;
    ProtobufAdapter<GameMessage> codec;

    GameMessage src;
    src.version = 5;
    src.body = "game-message-bridge";

    std::string payload;
    std::string err;
    assert(codec.encodeMessage(src, &payload, &err));

    const auto frame = framer.encode(kGameMsgId, 0, 0, payload);
    Packet packet;
    std::size_t consumed = 0;
    const auto state = framer.decode(frame.data(), frame.size(), packet, consumed);
    assert(state == PacketDecodeState::kComplete);
    assert(consumed == frame.size());
    assert(packet.header.msgId == kGameMsgId);
    assert(!packet.payload.empty());

    GameMessage dst;
    assert(codec.decodeMessage(packet.payload, dst, &err));
    assert(err.empty());
    assert(dst.version == src.version);
    assert(dst.body == src.body);

    std::printf("  PASS: task-08 integration — codec + framing roundtrip\n");
    std::printf("All game message roundtrip integration test passed.\n");
    return 0;
}
