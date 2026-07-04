// CodecAdapter — 标准化游戏消息序列化桥接接口。
//
// 目标：
// - 统一不同序列化框架（Protobuf / FlatBuffers / 自定义）与网络层对接方式；
// - 用统一的 encode/decode 错误语义减少协议代码重复；
// - 在 Task-07 的统一 PacketFramer 之后提供“消息对象 <-> payload bytes”桥接层。
//
// 线程模型：
// - 编解码器本身无状态（或保持内部只读状态）时，天然可跨线程共享；
// - 如果编码器持有可变状态，请由调用方保证外部同步或线程绑定。
//
// 主要语义：
// - 所有 encode/decode 不抛异常，返回 bool 并可返回 error string；
// - 对空指针参数进行显式判空并返回失败，避免未定义行为。

#pragma once

#include <exception>
#include <string>
#include <string_view>

namespace mini::codec {

namespace detail {

inline bool setCodecError(std::string* error, std::string_view message) {
    if (error) {
        *error = message;
    }
    return false;
}

inline bool setCodecExceptionError(std::string* error,
                                   std::string_view action,
                                   const std::exception& ex) {
    if (error) {
        *error = std::string(action) + " threw exception: " + ex.what();
    }
    return false;
}

inline bool setCodecUnknownExceptionError(std::string* error, std::string_view action) {
    if (error) {
        *error = std::string(action) + " threw unknown exception";
    }
    return false;
}

}  // namespace detail

/// 编码成功或失败的状态。失败时可从 error 参数获取明确信息。
enum class CodecStatus : bool {
    kError = false,
    kOk = true,
};

/// 底层编码器统一基类（类型擦除视图）。
/// 通过 base interface 支持 runtime 注入 / 接口分离；typed 适配器提供静态类型安全包装。
class CodecAdapter {
public:
    virtual ~CodecAdapter() = default;

    /// 编码器名称，用于埋点和问题定位。
    virtual std::string_view name() const noexcept = 0;

    /// 将任意消息对象序列化为 payload。
    /// @param message 指向消息对象（由具体子类进行 downcast / 解析）
    /// @param payload 输出 buffer；成功时可被覆盖
    /// @param error   失败原因（可空）
    /// @return true=成功，false=失败
    virtual bool encode(const void* message,
                       std::string* payload,
                       std::string* error) const = 0;

    /// 将 payload 反序列化回消息对象。
    /// @param payload 输入 bytes
    /// @param message 指向目标消息对象（由具体子类进行 downcast / 解析）
    /// @param error   失败原因（可空）
    /// @return true=成功，false=失败
    virtual bool decode(std::string_view payload,
                       void* message,
                       std::string* error) const = 0;
};

/// typed 适配器基类：将 message type 纳入编译期，让调用方获得类型安全的编码/解码 API。
/// - 对外仍保留 type-erased 的 encode/decode，以便在运行时注入到广播等模块。
template <typename MessageT>
class TypedCodecAdapter : public CodecAdapter {
public:
    using MessageType = MessageT;

    bool encode(const void* message, std::string* payload, std::string* error) const final {
        if (message == nullptr) {
            return detail::setCodecError(error, "CodecAdapter::encode called with null message");
        }
        if (payload == nullptr) {
            return detail::setCodecError(error, "CodecAdapter::encode called with null payload");
        }

        try {
            return encodeMessage(*static_cast<const MessageType*>(message), payload, error);
        } catch (const std::exception& ex) {
            return detail::setCodecExceptionError(error, "CodecAdapter::encode", ex);
        } catch (...) {
            return detail::setCodecUnknownExceptionError(error, "CodecAdapter::encode");
        }
    }

    bool decode(std::string_view payload, void* message, std::string* error) const final {
        if (message == nullptr) {
            return detail::setCodecError(error, "CodecAdapter::decode called with null message");
        }

        try {
            return decodeMessage(payload, *static_cast<MessageType*>(message), error);
        } catch (const std::exception& ex) {
            return detail::setCodecExceptionError(error, "CodecAdapter::decode", ex);
        } catch (...) {
            return detail::setCodecUnknownExceptionError(error, "CodecAdapter::decode");
        }
    }

    /// 编码到具体消息类型。
    virtual bool encodeMessage(const MessageType& message,
                              std::string* payload,
                              std::string* error) const = 0;

    /// 从 payload 解码到具体消息类型。
    virtual bool decodeMessage(std::string_view payload,
                              MessageType& message,
                              std::string* error) const = 0;
};

}  // namespace mini::codec
