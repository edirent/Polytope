#pragma once

#include "engine/execution/public/ChildOrder.h"
#include "engine/execution/public/ExecutionTypes.h"

#include <array>
#include <cstdint>
#include <string>

namespace trading_engine::execution {

inline constexpr std::uint16_t kMaxChildOrdersPerPlan = 16;

struct OrderPlan {
    PlanId plan_id = 0;

    std::uint64_t source_intent_id = 0;
    std::uint64_t approved_intent_id = 0;
    std::uint64_t reservation_id = 0;

    std::uint64_t bundle_id = 0;

    PlanStatus status = PlanStatus::Created;

    std::uint16_t order_count = 0;
    std::array<ChildOrder, kMaxChildOrdersPerPlan> orders{};

    std::int64_t max_total_cost_tick = 0;
    std::int64_t min_expected_edge_tick = 0;
    std::int64_t max_slippage_tick = 0;

    std::uint64_t created_ts_ns = 0;
    std::uint64_t expire_after_ns = 0;

    std::string idempotency_key;
};

}  // namespace trading_engine::execution
