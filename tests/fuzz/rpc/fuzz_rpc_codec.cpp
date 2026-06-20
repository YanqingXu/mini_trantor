#include "mini/rpc/RpcCodec.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    mini::rpc::RpcMessage message;
    std::size_t consumed = 0;
    (void)mini::rpc::codec::decode(reinterpret_cast<const char*>(data), size, message, consumed);
    (void)mini::rpc::codec::decodePayload(
        std::string_view(reinterpret_cast<const char*>(data), size),
        message);
    return 0;
}
