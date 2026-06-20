// Unit tests for ProtobufAdapter type bridging and type-erased path.

#include "mini/codec/ProtobufAdapter.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace mini::codec;

namespace {

struct FakeProtoMessage {
    std::string payload;
    bool encodeOk = true;
    bool decodeOk = true;

    bool SerializeToString(std::string* out) const {
        if (!encodeOk) {
            return false;
        }
        *out = payload;
        return true;
    }

    bool ParseFromString(const std::string& data) {
        if (!decodeOk) {
            return false;
        }
        payload = data;
        return true;
    }

    bool ParseFromArray(const void* data, int len) {
        if (!decodeOk) {
            return false;
        }
        payload.assign(static_cast<const char*>(data), static_cast<std::size_t>(len));
        return true;
    }
};

}  // namespace

int main() {
    ProtobufAdapter<FakeProtoMessage> codec;

    FakeProtoMessage src{"hello"};
    std::string payload;
    std::string err;

    // 1. typed codec encode / decode
    {
        assert(codec.encodeMessage(src, &payload, &err));
        assert(err.empty());
        assert(payload == "hello");

        FakeProtoMessage dst;
        assert(codec.decodeMessage(payload, dst, &err));
        assert(err.empty());
        assert(dst.payload == "hello");
        std::printf("  PASS: typed protobuf encode/decode\n");
    }

    // 2. type-erased codec encode / decode
    {
        CodecAdapter& erased = codec;
        FakeProtoMessage dst;

        assert(erased.encode(&src, &payload, &err));
        assert(err.empty());
        assert(payload == "hello");

        assert(erased.decode(payload, &dst, &err));
        assert(err.empty());
        assert(dst.payload == "hello");
        std::printf("  PASS: erased protobuf encode/decode\n");
    }

    // 3. codec name
    {
        assert(codec.name() == "protobuf");
        std::printf("  PASS: protobuf codec name\n");
    }

    // 4. encode failure should surface clear error
    {
        FakeProtoMessage bad;
        bad.encodeOk = false;

        assert(!codec.encodeMessage(bad, &payload, &err));
        assert(err == "protobuf encode failed");
        std::printf("  PASS: protobuf encode failure\n");
    }

    // 5. decode failure should surface clear error and use base API
    {
        FakeProtoMessage bad;
        bad.decodeOk = false;
        std::string failedPayload = "bad";

        assert(!codec.decodeMessage(failedPayload, bad, &err));
        assert(err == "protobuf decode failed");
        std::printf("  PASS: protobuf decode failure\n");
    }

    // 6. null checks on type-erased API
    {
        assert(!codec.encode(nullptr, &payload, &err));
        assert(err == "CodecAdapter::encode called with null message");
        assert(!codec.decode("x", nullptr, &err));
        assert(err == "CodecAdapter::decode called with null message");
        std::printf("  PASS: protobuf null guard\n");
    }

    std::printf("All protobuf adapter unit tests passed.\n");
    return 0;
}
