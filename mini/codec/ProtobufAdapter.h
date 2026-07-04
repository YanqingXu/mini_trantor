// ProtobufAdapter — 与 Protobuf 风格消息对象的无状态桥接器。
//
// 约束：
// - MessageT 需实现 SerializeToString(std::string*) -> bool
// - MessageT 需实现 ParseFromString(const std::string&) -> bool 或 ParseFromArray(const void*, int) -> bool
//
// 该适配器不直接依赖 protobuf 头文件，便于在不接入 protobuf 库的编译场景下保持可构建；
// 用户可直接用真实 protobuf message 类型实例化该模板。

#pragma once

#include "mini/codec/CodecAdapter.h"

#include <concepts>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

namespace mini::codec {

namespace detail {
template <typename MessageT>
concept ProtobufSerializable = requires(const MessageT& message, std::string* out) {
    { message.SerializeToString(out) } -> std::same_as<bool>;
};

template <typename MessageT>
concept ProtobufParsableFromString = requires(MessageT& message, const std::string& text) {
    { message.ParseFromString(text) } -> std::same_as<bool>;
};

template <typename MessageT>
concept ProtobufParsableFromArray = requires(MessageT& message, const void* data, int len) {
    { message.ParseFromArray(data, len) } -> std::same_as<bool>;
};
}  // namespace detail

template <typename MessageT>
class ProtobufAdapter : public TypedCodecAdapter<MessageT> {
public:
    static_assert(detail::ProtobufSerializable<MessageT>,
                  "ProtobufAdapter<MessageT> requires SerializeToString(std::string*) const");
    static_assert(detail::ProtobufParsableFromString<MessageT> ||
                      detail::ProtobufParsableFromArray<MessageT>,
                  "ProtobufAdapter<MessageT> requires ParseFromString(const std::string&) or "
                  "ParseFromArray(const void*, int)");

    std::string_view name() const noexcept override {
        return "protobuf";
    }

    bool encodeMessage(const MessageT& message,
                       std::string* payload,
                       std::string* error) const override {
        if (payload == nullptr) {
            return detail::setCodecError(error, "protobuf encode called with null payload");
        }

        bool ok = false;
        try {
            ok = message.SerializeToString(payload);
        } catch (const std::exception& ex) {
            return detail::setCodecExceptionError(error, "protobuf encode", ex);
        } catch (...) {
            return detail::setCodecUnknownExceptionError(error, "protobuf encode");
        }

        if (!ok) {
            return detail::setCodecError(error, "protobuf encode failed");
        }
        if (error) {
            error->clear();
        }
        return static_cast<bool>(CodecStatus::kOk);
    }

    bool decodeMessage(std::string_view payload,
                       MessageT& message,
                       std::string* error) const override {
        bool ok = false;
        try {
            if constexpr (detail::ProtobufParsableFromArray<MessageT>) {
                if (payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                    return detail::setCodecError(error, "protobuf decode payload too large");
                }
                ok = message.ParseFromArray(payload.data(), static_cast<int>(payload.size()));
            } else {
                const std::string payloadStr(payload);
                ok = message.ParseFromString(payloadStr);
            }
        } catch (const std::exception& ex) {
            return detail::setCodecExceptionError(error, "protobuf decode", ex);
        } catch (...) {
            return detail::setCodecUnknownExceptionError(error, "protobuf decode");
        }

        if (!ok) {
            return detail::setCodecError(error, "protobuf decode failed");
        }
        if (error) {
            error->clear();
        }
        return static_cast<bool>(CodecStatus::kOk);
    }
};

}  // namespace mini::codec
