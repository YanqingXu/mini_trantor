// Unit tests for FlatBuffersAdapter callback-based bridge.

#include "mini/codec/FlatBuffersAdapter.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace mini::codec;

namespace {

struct FakeFlatMessage {
    std::uint16_t id{0};
    std::string payload;
    bool encodeOk = true;
    bool decodeOk = true;
};

bool encodeFakeMessage(const FakeFlatMessage& msg, std::string* out) {
    if (!msg.encodeOk || out == nullptr) {
        return false;
    }

    out->resize(2 + msg.payload.size());
    (*out)[0] = static_cast<char>((msg.id >> 8) & 0xFF);
    (*out)[1] = static_cast<char>(msg.id & 0xFF);
    out->replace(2, std::string::npos, msg.payload);
    return true;
}

bool decodeFakeMessage(std::string_view payload, FakeFlatMessage& msg) {
    if (!msg.decodeOk || payload.size() < 2) {
        return false;
    }
    msg.id = (static_cast<std::uint16_t>(static_cast<unsigned char>(payload[0])) << 8) |
             static_cast<std::uint16_t>(static_cast<unsigned char>(payload[1]));
    msg.payload.assign(payload.data() + 2, payload.size() - 2);
    return true;
}

bool throwEncodeMessage(const FakeFlatMessage&, std::string*) {
    throw std::runtime_error("encode boom");
}

bool throwDecodeMessage(std::string_view, FakeFlatMessage&) {
    throw std::runtime_error("decode boom");
}

}  // namespace

int main() {
    FlatBuffersAdapter<FakeFlatMessage> codec;
    std::string payload;
    std::string err;

    // 1. codec name
    {
        assert(codec.name() == "flatbuffers");
        std::printf("  PASS: flatbuffers codec name\n");
    }

    // 2. failure when encode/decode handlers are missing
    {
        FakeFlatMessage msg;
        msg.id = 11;
        msg.payload = "hello";

        assert(!codec.encodeMessage(msg, &payload, &err));
        assert(err == "FlatBuffersAdapter has no encoder");

        assert(!codec.decodeMessage("xx", msg, &err));
        assert(err == "FlatBuffersAdapter has no decoder");
        std::printf("  PASS: missing flatbuffers handlers handled\n");
    }

    // 3. typed encode/decode with injected callbacks
    {
        FlatBuffersAdapter<FakeFlatMessage> codecWithFns{encodeFakeMessage, decodeFakeMessage};
        FakeFlatMessage src;
        src.id = 42;
        src.payload = "payload";

        assert(codecWithFns.encodeMessage(src, &payload, &err));
        assert(err.empty());
        assert(payload.size() == 2 + src.payload.size());

        FakeFlatMessage dst;
        dst.decodeOk = true;
        assert(codecWithFns.decodeMessage(payload, dst, &err));
        assert(err.empty());
        assert(dst.id == src.id);
        assert(dst.payload == src.payload);
        std::printf("  PASS: flatbuffers typed encode/decode\n");
    }

    // 4. type-erased API route
    {
        FlatBuffersAdapter<FakeFlatMessage> codecWithFns{encodeFakeMessage, decodeFakeMessage};
        FakeFlatMessage src;
        src.id = 7;
        src.payload = "game";
        std::string raw;

        CodecAdapter& erased = codecWithFns;
        assert(erased.encode(&src, &raw, &err));
        assert(err.empty());

        FakeFlatMessage dst;
        assert(erased.decode(raw, &dst, &err));
        assert(err.empty());
        assert(dst.id == src.id);
        assert(dst.payload == src.payload);
        std::printf("  PASS: flatbuffers erased encode/decode\n");
    }

    // 5. encode helper failure path
    {
        FlatBuffersAdapter<FakeFlatMessage> codecWithFns{encodeFakeMessage, decodeFakeMessage};
        FakeFlatMessage bad;
        bad.encodeOk = false;
        assert(!codecWithFns.encodeMessage(bad, &payload, &err));
        assert(err == "flatbuffers encode failed");
        assert(!codecWithFns.encodeMessage(bad, nullptr, &err));
        assert(err == "flatbuffers encode called with null payload");

        FakeFlatMessage badDecode;
        badDecode.decodeOk = false;
        assert(!codecWithFns.decodeMessage("", badDecode, &err));
        assert(err == "flatbuffers decode failed");
        std::printf("  PASS: callback failure path\n");
    }

    // 6. injected callback exceptions should become explicit errors
    {
        FlatBuffersAdapter<FakeFlatMessage> codecWithThrowingFns{
            throwEncodeMessage,
            throwDecodeMessage};
        FakeFlatMessage msg;

        assert(!codecWithThrowingFns.encodeMessage(msg, &payload, &err));
        assert(err.rfind("flatbuffers encode threw exception", 0) == 0);

        assert(!codecWithThrowingFns.decodeMessage("xx", msg, &err));
        assert(err.rfind("flatbuffers decode threw exception", 0) == 0);
        std::printf("  PASS: flatbuffers exception guard\n");
    }

    std::printf("All flatbuffers adapter unit tests passed.\n");
    return 0;
}
