// Contract tests for serialization bridge contracts.
// Focus on: errors are explicit, null checks are safe, codec names are stable.

#include "mini/codec/ProtobufAdapter.h"

#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace mini::codec;

namespace {

struct FakeProtoMessage {
    std::string payload;

    bool SerializeToString(std::string* out) const {
        if (payload.empty()) {
            return false;
        }
        *out = payload;
        return true;
    }

    bool ParseFromString(const std::string& data) {
        payload = data;
        return true;
    }
};

struct ThrowingProtoMessage {
    bool SerializeToString(std::string*) const {
        throw std::runtime_error("encode failed hard");
    }

    bool ParseFromString(const std::string&) {
        throw std::runtime_error("decode failed hard");
    }
};

}  // namespace

int main() {
    ProtobufAdapter<FakeProtoMessage> codec;

    // Contract 1: codec name should be stable.
    {
        assert(codec.name() == "protobuf");
        std::printf("  PASS: codec name is stable\n");
    }

    // Contract 2: base API must refuse null pointers.
    {
        std::string payload;
        std::string err;

        assert(!codec.encode(nullptr, &payload, &err));
        assert(err == "CodecAdapter::encode called with null message");

        FakeProtoMessage msg{"hello"};
        assert(!codec.decode(payload, nullptr, &err));
        assert(err == "CodecAdapter::decode called with null message");

        std::printf("  PASS: null-pointer guard contract\n");
    }

    // Contract 3: successful round-trip keeps semantic content.
    {
        FakeProtoMessage src{"roundtrip"};
        std::string payload;
        std::string err;

        assert(codec.encodeMessage(src, &payload, &err));
        assert(err.empty());

        FakeProtoMessage dst;
        assert(codec.decodeMessage(payload, dst, &err));
        assert(err.empty());
        assert(dst.payload == src.payload);

        std::printf("  PASS: round-trip contract\n");
    }

    // Contract 4: encode failure should not silently succeed.
    {
        FakeProtoMessage empty;
        std::string payload;
        std::string err;
        assert(!codec.encodeMessage(empty, &payload, &err));
        assert(!err.empty());
        std::printf("  PASS: encode-fail contract\n");
    }

    // Contract 5: serializer exceptions are converted into explicit errors.
    {
        ProtobufAdapter<ThrowingProtoMessage> throwingCodec;
        CodecAdapter& erased = throwingCodec;
        ThrowingProtoMessage msg;
        std::string payload;
        std::string err;

        assert(!erased.encode(&msg, &payload, &err));
        assert(err.rfind("protobuf encode threw exception", 0) == 0);

        assert(!erased.decode("payload", &msg, &err));
        assert(err.rfind("protobuf decode threw exception", 0) == 0);
        std::printf("  PASS: exception-to-error contract\n");
    }

    std::printf("All codec adapter contract tests passed.\n");
    return 0;
}
