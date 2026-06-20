#include "mini/ws/WebSocketCodec.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    mini::ws::WsFrame frame;
    std::size_t consumed = 0;
    (void)mini::ws::codec::decode(reinterpret_cast<const char*>(data), size, frame, consumed);
    return 0;
}
