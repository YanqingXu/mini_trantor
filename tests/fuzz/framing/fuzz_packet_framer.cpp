#include "mini/net/framing/PacketFramer.h"

#include <array>
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    mini::net::framing::PacketFramer framer;
    mini::net::framing::Packet packet;
    std::size_t consumed = 0;
    const char* bytes = reinterpret_cast<const char*>(data);

    (void)framer.decode(bytes, size, packet, consumed);

    std::array<mini::net::framing::Packet, 4> packets{};
    (void)framer.decodeBatch(bytes, size, packets.data(), packets.size());
    return 0;
}
