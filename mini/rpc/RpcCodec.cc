#include "mini/rpc/RpcCodec.h"

namespace mini::rpc::codec {

namespace {

constexpr std::uint16_t kRpcFrameFlags = 0;
const mini::net::framing::PacketFramer kFrameFramer;

void appendUint16(std::string& buf, std::uint16_t val) {
    buf.push_back(static_cast<char>((val >> 8) & 0xFF));
    buf.push_back(static_cast<char>(val & 0xFF));
}

void appendUint64(std::string& buf, std::uint64_t val) {
    for (int i = 7; i >= 0; --i) {
        buf.push_back(static_cast<char>((val >> (i * 8)) & 0xFF));
    }
}

std::uint64_t readUint64(const char* p) {
    auto* b = reinterpret_cast<const std::uint8_t*>(p);
    std::uint64_t val = 0;
    for (int i = 0; i < 8; ++i) {
        val = (val << 8) | b[i];
    }
    return val;
}

std::uint16_t readUint16(const char* p) {
    auto* b = reinterpret_cast<const std::uint8_t*>(p);
    return static_cast<std::uint16_t>((b[0] << 8) | b[1]);
}

std::string encodeFrame(std::uint64_t requestId,
                        RpcMsgType msgType,
                        std::string_view method,
                        std::string_view payload) {
    const std::uint32_t methodLen = static_cast<std::uint32_t>(method.size());
    const std::size_t bodyLen = 8 + 1 + 2 + method.size() + payload.size();
    if (bodyLen > kMaxFrameBodySize) {
        return {};
    }

    std::string body;
    body.reserve(bodyLen);
    appendUint64(body, requestId);
    body.push_back(static_cast<char>(msgType));
    appendUint16(body, static_cast<std::uint16_t>(methodLen));
    body.append(method.data(), method.size());
    body.append(payload.data(), payload.size());

    return kFrameFramer.encode(kRpcMsgId, kRpcFrameFlags, 0, body);
}

RpcDecodeResult decodeBody(std::string_view payload,
                          RpcMessage& msg) {
    const auto* body = payload.data();
    const auto bodyLen = payload.size();
    if (bodyLen < kMinBodyLen) {
        return RpcDecodeResult::kError;
    }

    msg.requestId = readUint64(body);
    const auto rawType = static_cast<std::uint8_t>(body[8]);
    if (rawType > static_cast<std::uint8_t>(RpcMsgType::kError)) {
        return RpcDecodeResult::kError;
    }
    msg.msgType = static_cast<RpcMsgType>(rawType);

    const std::uint16_t methodLen = readUint16(body + 9);
    const std::size_t expectedDataLen = 8 + 1 + 2 + methodLen;
    if (expectedDataLen > bodyLen) {
        return RpcDecodeResult::kError;
    }

    msg.method.assign(body + 11, methodLen);
    const std::size_t payloadOffset = 11 + methodLen;
    msg.payload.assign(body + payloadOffset, bodyLen - payloadOffset);
    return RpcDecodeResult::kComplete;
}

}  // namespace

std::string encodeRequest(std::uint64_t requestId,
                          std::string_view method,
                          std::string_view payload) {
    return encodeFrame(requestId, RpcMsgType::kRequest, method, payload);
}

std::string encodeResponse(std::uint64_t requestId,
                           std::string_view payload) {
    return encodeFrame(requestId, RpcMsgType::kResponse, "", payload);
}

std::string encodeError(std::uint64_t requestId,
                        std::string_view errorMessage) {
    return encodeFrame(requestId, RpcMsgType::kError, "", errorMessage);
}

RpcDecodeResult decode(const char* data, std::size_t len,
                       RpcMessage& msg, std::size_t& consumed) {
    mini::net::framing::Packet packet;
    auto state = kFrameFramer.decode(data, len, packet, consumed);
    if (state == mini::net::framing::PacketDecodeState::kNeedMore) {
        return RpcDecodeResult::kIncomplete;
    }
    if (state != mini::net::framing::PacketDecodeState::kComplete) {
        consumed = 0;
        return RpcDecodeResult::kError;
    }
    if (packet.header.msgId != kRpcMsgId) {
        consumed = 0;
        return RpcDecodeResult::kError;
    }

    const auto bodyResult = decodeBody(packet.payload, msg);
    if (bodyResult != RpcDecodeResult::kComplete) {
        consumed = 0;
        return RpcDecodeResult::kError;
    }

    return bodyResult;
}

RpcDecodeResult decodePayload(std::string_view payload, RpcMessage& msg) {
    return decodeBody(payload, msg);
}

}  // namespace mini::rpc::codec
