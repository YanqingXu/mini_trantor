// FlatBuffersAdapter — 通用 FlatBuffers 风格桥接器。
//
// FlatBuffers 本身的 API 差异较大，本适配器采用可注入 encode/decode 回调的方式进行对接：
//  - encode(message, payload) 负责将对象序列化为二进制 payload；
//  - decode(payload, message) 负责将 payload 反序列化到对象。
//
// 优点：
// - 不直接引入 flatbuffers 依赖；
// - 统一了 GameMessage 与网络层之间的编解码入口；
// - 仅需在接入层注入与具体 .fbs schema 对应的序列化函数。

#pragma once

#include "mini/codec/CodecAdapter.h"

#include <functional>
#include <string>
#include <string_view>

namespace mini::codec {

template <typename MessageT>
class FlatBuffersAdapter : public TypedCodecAdapter<MessageT> {
public:
    using EncodeFn = std::function<bool(const MessageT&, std::string*)>;
    using DecodeFn = std::function<bool(std::string_view, MessageT&)>;

    FlatBuffersAdapter() = default;

    FlatBuffersAdapter(EncodeFn encoder, DecodeFn decoder)
        : encoder_(std::move(encoder)),
          decoder_(std::move(decoder)) {
    }

    std::string_view name() const noexcept override {
        return "flatbuffers";
    }

    bool encodeMessage(const MessageT& message,
                       std::string* payload,
                       std::string* error) const override {
        if (payload == nullptr) {
            return detail::setCodecError(error, "flatbuffers encode called with null payload");
        }
        if (!encoder_) {
            return detail::setCodecError(error, "FlatBuffersAdapter has no encoder");
        }

        bool ok = false;
        try {
            ok = encoder_(message, payload);
        } catch (const std::exception& ex) {
            return detail::setCodecExceptionError(error, "flatbuffers encode", ex);
        } catch (...) {
            return detail::setCodecUnknownExceptionError(error, "flatbuffers encode");
        }

        if (!ok) {
            return detail::setCodecError(error, "flatbuffers encode failed");
        }

        if (error) {
            error->clear();
        }
        return static_cast<bool>(CodecStatus::kOk);
    }

    bool decodeMessage(std::string_view payload,
                       MessageT& message,
                       std::string* error) const override {
        if (!decoder_) {
            return detail::setCodecError(error, "FlatBuffersAdapter has no decoder");
        }

        bool ok = false;
        try {
            ok = decoder_(payload, message);
        } catch (const std::exception& ex) {
            return detail::setCodecExceptionError(error, "flatbuffers decode", ex);
        } catch (...) {
            return detail::setCodecUnknownExceptionError(error, "flatbuffers decode");
        }

        if (!ok) {
            return detail::setCodecError(error, "flatbuffers decode failed");
        }

        if (error) {
            error->clear();
        }
        return static_cast<bool>(CodecStatus::kOk);
    }

private:
    EncodeFn encoder_;
    DecodeFn decoder_;
};

}  // namespace mini::codec
