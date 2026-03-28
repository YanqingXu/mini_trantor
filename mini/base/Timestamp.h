#pragma once

#include <chrono>

namespace mini::base {

using Timestamp = std::chrono::steady_clock::time_point;

inline Timestamp now() {
    return std::chrono::steady_clock::now();
}

}  // namespace mini::base
