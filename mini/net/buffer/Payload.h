// Payload — 广播链路共享载荷。
// 目标：通过共享句柄贯通广播路径，避免每次分发重复构造字符串。

#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace mini::net::buffer {

class Payload {
public:
    Payload() = default;
    explicit Payload(std::string data);

    Payload(const Payload&) = delete;
    Payload& operator=(const Payload&) = delete;

    std::string_view view() const noexcept;
    bool empty() const noexcept;
    std::size_t size() const noexcept;

private:
    friend class PayloadPool;

    void reset(std::string_view data);
    void reset(std::string&& data);
    void clear();

    std::string data_;
};

using PayloadPtr = std::shared_ptr<Payload>;

}  // namespace mini::net::buffer
