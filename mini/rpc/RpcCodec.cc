#include "mini/rpc/RpcCodec.h"

#include <cstring>

namespace mini::rpc::codec {

namespace {

void appendUint64(std::string& buf, std::uint64_t val) {
    for (int i = 7; i >= 0; --i) {
        buf.push_back(static_cast<char>((val >> (i * 8)) & 0xFF));
    }
}

void appendUint32(std::string& buf, std::uint32_t val) {
    buf.push_back(static_cast<char>((val >> 24) & 0xFF));
    buf.push_back(static_cast<char>((val >> 16) & 0xFF));
    buf.push_back(static_cast<char>((val >> 8) & 0xFF));
    buf.push_back(static_cast<char>(val & 0xFF));
}

void appendUint16(std::string& buf, std::uint16_t val) {
    buf.push_back(static_cast<char>((val >> 8) & 0xFF));
    buf.push_back(static_cast<char>(val & 0xFF));
}

std::uint64_t readUint64(const char* p) {
    auto* b = reinterpret_cast<const std::uint8_t*>(p);
    std::uint64_t val = 0;
    for (int i = 0; i < 8; ++i) {
        val = (val << 8) | b[i];
    }
    return val;
}

std::uint32_t readUint32(const char* p) {
    auto* b = reinterpret_cast<const std::uint8_t*>(p);
    return (static_cast<std::uint32_t>(b[0]) << 24) |
           (static_cast<std::uint32_t>(b[1]) << 16) |
           (static_cast<std::uint32_t>(b[2]) << 8) |
           static_cast<std::uint32_t>(b[3]);
}

std::uint16_t readUint16(const char* p) {
    auto* b = reinterpret_cast<const std::uint8_t*>(p);
    return static_cast<std::uint16_t>((b[0] << 8) | b[1]);
}

std::string encodeFrame(std::uint64_t requestId,
                        RpcMsgType msgType,
                        std::string_view method,
                        std::string_view payload) {
    // body = requestId(8) + msgType(1) + methodLen(2) + method + payload
    std::uint16_t methodLen = static_cast<std::uint16_t>(method.size());
    std::uint32_t bodyLen = 8 + 1 + 2 +
                            static_cast<std::uint32_t>(method.size()) +
                            static_cast<std::uint32_t>(payload.size());

    std::string frame;
    frame.reserve(kHeaderLen + bodyLen);

    appendUint32(frame, bodyLen);
    appendUint64(frame, requestId);
    frame.push_back(static_cast<char>(msgType));
    appendUint16(frame, methodLen);
    frame.append(method.data(), method.size());
    frame.append(payload.data(), payload.size());
    return frame;
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
    if (len < kHeaderLen) return RpcDecodeResult::kIncomplete;

    std::uint32_t bodyLen = readUint32(data);

    // Safety limit
    if (bodyLen > kMaxFrameBodySize) return RpcDecodeResult::kError;

    // Minimum body: requestId(8) + msgType(1) + methodLen(2) = 11
    if (bodyLen < kMinBodyLen) return RpcDecodeResult::kError;

    std::size_t totalLen = kHeaderLen + bodyLen;
    if (len < totalLen) return RpcDecodeResult::kIncomplete;

    const char* body = data + kHeaderLen;

    msg.requestId = readUint64(body);
    auto rawType = static_cast<std::uint8_t>(body[8]);
    if (rawType > static_cast<std::uint8_t>(RpcMsgType::kError)) {
        return RpcDecodeResult::kError;
    }
    msg.msgType = static_cast<RpcMsgType>(rawType);

    std::uint16_t methodLen = readUint16(body + 9);

    // Ensure methodLen fits within the remaining body
    std::size_t expectedDataLen = 8 + 1 + 2 + methodLen;
    if (expectedDataLen > bodyLen) return RpcDecodeResult::kError;

    msg.method.assign(body + 11, methodLen);

    std::size_t payloadOffset = 11 + methodLen;
    std::size_t payloadLen = bodyLen - payloadOffset;
    msg.payload.assign(body + payloadOffset, payloadLen);

    consumed = totalLen;
    return RpcDecodeResult::kComplete;
}

}  // namespace mini::rpc::codec
