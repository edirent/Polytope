#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::execution {

using ExecutionId = std::uint64_t;
using PlanId = std::uint64_t;
using ChildOrderId = std::uint64_t;

enum class ExecutionMode : std::uint8_t {
    Paper,
    Sandbox,
    Live
};

enum class OrderSide : std::uint8_t {
    Buy,
    Sell
};

enum class OrderType : std::uint8_t {
    Limit,
    Market
};

enum class TimeInForce : std::uint8_t {
    Gtc,
    Ioc,
    Fok
};

enum class PaperExecutionMode : std::uint8_t {
    PaperAtomic,
    PaperSequential
};

enum class ChildOrderStatus : std::uint8_t {
    Created,
    New = Created,
    Planned,
    Sent,
    Submitted = Sent,
    Acked,
    PartiallyFilled,
    Filled,
    CancelRequested,
    Cancelled,
    Canceled = Cancelled,
    Failed,
    Rejected = Failed,
    Expired
};

enum class PlanStatus : std::uint8_t {
    Created,
    Planned,
    Sent,
    Submitted = Sent,
    Acked,
    PartiallyFilled,
    HedgeRequired,
    Filled,
    CancelRequested,
    Canceling = CancelRequested,
    Cancelled,
    Canceled = Cancelled,
    Failed,
    Rejected = Failed,
    Expired
};

[[nodiscard]] const char* to_string(ExecutionMode mode) noexcept;
[[nodiscard]] const char* to_string(OrderSide side) noexcept;
[[nodiscard]] const char* to_string(ChildOrderStatus status) noexcept;
[[nodiscard]] const char* to_string(PlanStatus status) noexcept;

}  // namespace trading_engine::execution
