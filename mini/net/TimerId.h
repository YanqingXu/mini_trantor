#pragma once

// TimerId 是 TimerQueue 对外暴露的取消句柄。
// 它只承载稳定的序号身份，不暴露内部定时器对象地址。

#include <cstdint>

namespace mini::net {

class TimerQueue;

class TimerId {
public:
    TimerId() noexcept = default;

    bool valid() const noexcept {
        return sequence_ != 0;
    }

private:
    explicit TimerId(std::int64_t sequence) noexcept : sequence_(sequence) {
    }

    std::int64_t sequence_{0};

    friend class TimerQueue;
};

}  // namespace mini::net
