#pragma once

#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/OrderPlan.h"

#include <array>
#include <cstdint>

namespace trading_engine::execution {

struct ChildFillState {
    std::uint64_t order_id = 0;

    std::int64_t requested_qty_lots = 0;
    std::int64_t filled_qty_lots = 0;
    std::int64_t remaining_qty_lots = 0;

    std::int64_t total_cost_tick = 0;
    std::int64_t avg_fill_price_tick = 0;
};

struct PlanFillState {
    std::uint64_t plan_id = 0;

    std::uint16_t child_count = 0;
    std::array<ChildFillState, kMaxChildOrdersPerPlan> children{};

    bool any_filled = false;
    bool all_filled = false;
    bool any_partial = false;

    std::int64_t total_cost_tick = 0;
};

struct FillTrackerState {
    std::int64_t filled_lots = 0;
    std::int64_t avg_fill_price_tick = 0;
};

class FillTracker {
public:
    FillTracker() = default;
    explicit FillTracker(const OrderPlan& plan);

    void reset(const OrderPlan& plan);
    void apply(const ExecutionReport& report);

    [[nodiscard]] FillTrackerState state() const noexcept;
    [[nodiscard]] const PlanFillState& plan_state() const noexcept;
    [[nodiscard]] const ChildFillState* find_child(
        ChildOrderId order_id
    ) const noexcept;

private:
    FillTrackerState state_;
    PlanFillState plan_state_;
};

}  // namespace trading_engine::execution
