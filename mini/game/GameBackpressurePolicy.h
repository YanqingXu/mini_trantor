#pragma once

// GameBackpressurePolicy 定义游戏网络路径的背压配置。
// 它是值语义配置载体，不拥有任何 loop / connection / session；
// 具体 enforcement 必须在受保护资源的 owner loop 上执行。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mini::game {

enum class GameMessagePriority : std::uint32_t {
    Low = 0,
    Normal = 1,
    High = 2,
    Critical = 3,
};

inline constexpr std::uint16_t kGamePriorityFlagMask = 0x0003;
inline constexpr std::uint16_t kGamePriorityFlagNormal = 0x0000;
inline constexpr std::uint16_t kGamePriorityFlagLow = 0x0001;
inline constexpr std::uint16_t kGamePriorityFlagHigh = 0x0002;
inline constexpr std::uint16_t kGamePriorityFlagCritical = 0x0003;

inline std::uint32_t toMetricPriority(GameMessagePriority priority) noexcept {
    return static_cast<std::uint32_t>(priority);
}

inline GameMessagePriority priorityFromMetric(std::uint32_t priority) noexcept {
    if (priority <= toMetricPriority(GameMessagePriority::Critical)) {
        return static_cast<GameMessagePriority>(priority);
    }
    return GameMessagePriority::Critical;
}

inline GameMessagePriority priorityFromPacketFlags(std::uint16_t flags) noexcept {
    switch (flags & kGamePriorityFlagMask) {
    case kGamePriorityFlagLow:
        return GameMessagePriority::Low;
    case kGamePriorityFlagHigh:
        return GameMessagePriority::High;
    case kGamePriorityFlagCritical:
        return GameMessagePriority::Critical;
    case kGamePriorityFlagNormal:
    default:
        return GameMessagePriority::Normal;
    }
}

struct GameBackpressureOptions {
    using Duration = std::chrono::steady_clock::duration;

    struct PriorityShedding {
        GameMessagePriority softLimitMinPriority{GameMessagePriority::Low};
        bool adaptiveSoftLimit{false};

        static bool validPriority(GameMessagePriority priority) noexcept {
            return priority == GameMessagePriority::Low ||
                   priority == GameMessagePriority::Normal ||
                   priority == GameMessagePriority::High ||
                   priority == GameMessagePriority::Critical;
        }

        void validate(std::string_view fieldName) const {
            if (!validPriority(softLimitMinPriority)) {
                throw std::invalid_argument(
                    std::string(fieldName) + ": invalid soft-limit minimum priority");
            }
        }

        bool enabled() const noexcept {
            return adaptiveSoftLimit || softLimitMinPriority != GameMessagePriority::Low;
        }

        std::size_t effectiveSoftLimit(std::size_t softLimit,
                                       std::size_t hardLimit) const noexcept {
            if (softLimit > 0) {
                return softLimit;
            }
            if (!adaptiveSoftLimit || hardLimit == 0) {
                return 0;
            }
            return hardLimit > 1 ? hardLimit / 2 : hardLimit;
        }

        Duration effectiveSoftLimit(Duration softLimit,
                                    Duration hardLimit) const noexcept {
            if (softLimit > Duration::zero()) {
                return softLimit;
            }
            if (!adaptiveSoftLimit || hardLimit <= Duration::zero()) {
                return Duration::zero();
            }
            return hardLimit / 2;
        }

        GameMessagePriority requiredPriority(std::size_t currentValue,
                                             std::size_t softLimit,
                                             std::size_t hardLimit) const noexcept {
            const auto effectiveSoft = effectiveSoftLimit(softLimit, hardLimit);
            if (effectiveSoft == 0 || currentValue < effectiveSoft) {
                return GameMessagePriority::Low;
            }
            return escalateIfNeeded(currentValue, effectiveSoft, hardLimit);
        }

        GameMessagePriority requiredPriority(Duration currentValue,
                                             Duration softLimit,
                                             Duration hardLimit) const noexcept {
            const auto effectiveSoft = effectiveSoftLimit(softLimit, hardLimit);
            if (effectiveSoft <= Duration::zero() || currentValue < effectiveSoft) {
                return GameMessagePriority::Low;
            }
            return escalateIfNeeded(currentValue, effectiveSoft, hardLimit);
        }

        bool shouldDrop(std::uint32_t priority,
                        std::size_t currentValue,
                        std::size_t softLimit,
                        std::size_t hardLimit) const noexcept {
            return priorityFromMetric(priority) < requiredPriority(currentValue, softLimit, hardLimit);
        }

        bool shouldDrop(std::uint32_t priority,
                        Duration currentValue,
                        Duration softLimit,
                        Duration hardLimit) const noexcept {
            return priorityFromMetric(priority) < requiredPriority(currentValue, softLimit, hardLimit);
        }

    private:
        template <typename T>
        GameMessagePriority escalateIfNeeded(T currentValue,
                                             T effectiveSoft,
                                             T hardLimit) const noexcept {
            auto required = softLimitMinPriority;
            if (!adaptiveSoftLimit ||
                hardLimit <= effectiveSoft ||
                required == GameMessagePriority::Critical) {
                return required;
            }

            const auto escalationPoint = effectiveSoft + ((hardLimit - effectiveSoft) / 2);
            if (currentValue >= escalationPoint) {
                required = static_cast<GameMessagePriority>(toMetricPriority(required) + 1);
            }
            return required;
        }
    };

    static void validateSizePair(std::size_t softLimit,
                                 std::size_t hardLimit,
                                 std::string_view fieldName) {
        if (hardLimit == 0) {
            if (softLimit != 0) {
                throw std::invalid_argument(
                    std::string(fieldName) + ": soft limit requires a non-zero hard limit");
            }
            return;
        }
        if (softLimit > hardLimit) {
            throw std::invalid_argument(
                std::string(fieldName) + ": soft limit must be <= hard limit");
        }
    }

    static void validateDurationPair(Duration softLimit,
                                     Duration hardLimit,
                                     std::string_view fieldName) {
        if (softLimit < Duration::zero() || hardLimit < Duration::zero()) {
            throw std::invalid_argument(
                std::string(fieldName) + ": limits must be non-negative");
        }
        if (hardLimit == Duration::zero()) {
            if (softLimit != Duration::zero()) {
                throw std::invalid_argument(
                    std::string(fieldName) + ": soft limit requires a non-zero hard limit");
            }
            return;
        }
        if (softLimit > hardLimit) {
            throw std::invalid_argument(
                std::string(fieldName) + ": soft limit must be <= hard limit");
        }
    }

    struct InputFraming {
        std::size_t softBufferedBytes{0};
        std::size_t hardBufferedBytes{0};

        void validate() const {
            validateSizePair(
                softBufferedBytes,
                hardBufferedBytes,
                "GameBackpressureOptions::InputFraming::bufferedBytes");
        }
    };

    struct LogicAdmission {
        std::size_t softBacklog{0};
        std::size_t hardBacklog{0};
        Duration softOldestLag{Duration::zero()};
        Duration hardOldestLag{Duration::zero()};

        void validate() const {
            validateSizePair(
                softBacklog,
                hardBacklog,
                "GameBackpressureOptions::LogicAdmission::backlog");
            validateDurationPair(
                softOldestLag,
                hardOldestLag,
                "GameBackpressureOptions::LogicAdmission::oldestLag");
        }
    };

    struct OutputSend {
        std::size_t softQueuedBytes{0};
        std::size_t hardQueuedBytes{0};
        Duration softQueueLatency{Duration::zero()};
        Duration hardQueueLatency{Duration::zero()};
        PriorityShedding priority{};

        void validate() const {
            validateSizePair(
                softQueuedBytes,
                hardQueuedBytes,
                "GameBackpressureOptions::OutputSend::queuedBytes");
            validateDurationPair(
                softQueueLatency,
                hardQueueLatency,
                "GameBackpressureOptions::OutputSend::queueLatency");
            priority.validate("GameBackpressureOptions::OutputSend::priority");
            if (priority.enabled() &&
                hardQueuedBytes == 0 &&
                hardQueueLatency == Duration::zero()) {
                throw std::invalid_argument(
                    "GameBackpressureOptions::OutputSend::priority requires an output hard limit");
            }
            if (priority.enabled() &&
                !priority.adaptiveSoftLimit &&
                softQueuedBytes == 0 &&
                softQueueLatency == Duration::zero()) {
                throw std::invalid_argument(
                    "GameBackpressureOptions::OutputSend::priority requires a soft limit or adaptive soft limit");
            }
        }
    };

    struct BroadcastFanout {
        std::size_t softFanoutConnections{0};
        std::size_t hardFanoutConnections{0};
        std::size_t softPayloadBytes{0};
        std::size_t hardPayloadBytes{0};
        PriorityShedding priority{};

        void validate() const {
            validateSizePair(
                softFanoutConnections,
                hardFanoutConnections,
                "GameBackpressureOptions::BroadcastFanout::fanoutConnections");
            validateSizePair(
                softPayloadBytes,
                hardPayloadBytes,
                "GameBackpressureOptions::BroadcastFanout::payloadBytes");
            priority.validate("GameBackpressureOptions::BroadcastFanout::priority");
            if (priority.enabled() &&
                hardFanoutConnections == 0 &&
                hardPayloadBytes == 0) {
                throw std::invalid_argument(
                    "GameBackpressureOptions::BroadcastFanout::priority requires a broadcast hard limit");
            }
            if (priority.enabled() &&
                !priority.adaptiveSoftLimit &&
                softFanoutConnections == 0 &&
                softPayloadBytes == 0) {
                throw std::invalid_argument(
                    "GameBackpressureOptions::BroadcastFanout::priority requires a soft limit or adaptive soft limit");
            }
        }
    };

    InputFraming input{};
    LogicAdmission logic{};
    OutputSend output{};
    BroadcastFanout broadcast{};

    void validate() const {
        input.validate();
        logic.validate();
        output.validate();
        broadcast.validate();
    }

    bool enabled() const noexcept {
        return input.hardBufferedBytes > 0 ||
               logic.hardBacklog > 0 ||
               logic.hardOldestLag > Duration::zero() ||
               output.hardQueuedBytes > 0 ||
               output.hardQueueLatency > Duration::zero() ||
               broadcast.hardFanoutConnections > 0 ||
               broadcast.hardPayloadBytes > 0;
    }

};

}  // namespace mini::game
