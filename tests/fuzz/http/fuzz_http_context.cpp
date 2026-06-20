#include "mini/http/HttpContext.h"
#include "mini/net/Buffer.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    mini::http::HttpContext context;
    mini::net::Buffer buffer;
    buffer.append(std::string_view(reinterpret_cast<const char*>(data), size));

    (void)context.parseRequest(&buffer);
    if (context.gotAll()) {
        context.reset();
    }

    return 0;
}
