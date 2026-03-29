#pragma once

// Timestamp 统一表示 reactor 内部使用的单调时钟时间点。
// 它只承担轻量时间戳语义，不引入额外的业务时间抽象。

#include <chrono>

namespace mini::base {

using Timestamp = std::chrono::steady_clock::time_point;

inline Timestamp now() {
    return std::chrono::steady_clock::now();
}

}  // namespace mini::base
