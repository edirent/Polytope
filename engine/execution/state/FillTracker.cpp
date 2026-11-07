#include "engine/execution/state/FillTracker.h"

#include <algorithm>

namespace trading_engine::execution {

namespace {

void refresh_plan_flags(PlanFillState* state) {
    state->any_filled = false;
    state->all_filled = state->child_count > 0;
    state->any_partial = false;
    state->total_cost_tick = 0;

    for (std::uint16_t i = 0; i < state->child_count; ++i) {
        const auto& child = state->children[i];
        state->any_filled = state->any_filled || child.filled_qty_lots > 0;
        state->all_filled = state->all_filled &&
            child.requested_qty_lots > 0 &&
            child.filled_qty_lots >= child.requested_qty_lots;
        state->any_partial = state->any_partial ||
            (child.filled_qty_lots > 0 &&
             child.filled_qty_lots < child.requested_qty_lots);
        state->total_cost_tick += child.total_cost_tick;
    }
}

}  // namespace

FillTracker::FillTracker(const OrderPlan& plan) {
    reset(plan);
}

void FillTracker::reset(const OrderPlan& plan) {
    applied_reports_.clear();
    state_ = {};
    plan_state_ = {};
    plan_state_.plan_id = plan.plan_id;
    plan_state_.child_count = std::min(
        plan.order_count,
        kMaxChildOrdersPerPlan
    );

    for (std::uint16_t i = 0; i < plan_state_.child_count; ++i) {
        auto& child = plan_state_.children[i];
        child.order_id = plan.orders[i].order_id;
        child.requested_qty_lots = plan.orders[i].quantity_lots;
        child.remaining_qty_lots = plan.orders[i].quantity_lots;
    }

    refresh_plan_flags(&plan_state_);
}

void FillTracker::apply(const ExecutionReport& report) {
    // ExecutionReport quantities are deltas. Suppress exact redelivery before
    // mutating either aggregate state or cost. Cumulative exchange counters must
    // first be normalized by the venue adapter.
    if (report.plan_id != plan_state_.plan_id || report.filled_lots <= 0 ||
        (report.status != ChildOrderStatus::Filled &&
         report.status != ChildOrderStatus::PartiallyFilled)) return;
    const auto key = !report.fill_id.empty() ? report.fill_id : std::to_string(report.plan_id) + ":" +
        std::to_string(report.child_order_id) + ":" + std::to_string(report.event_ts_ns) + ":" +
        std::to_string(static_cast<unsigned>(report.status)) + ":" +
        std::to_string(report.filled_lots) + ":" + std::to_string(report.avg_fill_price_tick);
    if (applied_reports_.contains(key)) return;
    ChildFillState* child = nullptr;
    for (std::uint16_t i = 0; i < plan_state_.child_count; ++i) {
        if (plan_state_.children[i].order_id == report.child_order_id) {
            child = &plan_state_.children[i];
            break;
        }
    }
    if (child == nullptr) {
        return;
    }

    if (report.filled_lots > child->remaining_qty_lots || report.avg_fill_price_tick < 0) return;
    applied_reports_.insert(key);
    const auto fill_lots = report.filled_lots;
    state_.filled_lots += fill_lots;
    state_.avg_fill_price_tick = report.avg_fill_price_tick;

    child->filled_qty_lots = std::min(
        child->requested_qty_lots,
        child->filled_qty_lots + fill_lots
    );
    child->remaining_qty_lots = std::max<std::int64_t>(
        0,
        child->requested_qty_lots - child->filled_qty_lots
    );
    if (report.remaining_lots > 0 ||
        child->filled_qty_lots >= child->requested_qty_lots) {
        child->remaining_qty_lots = std::min(
            child->remaining_qty_lots,
            std::max<std::int64_t>(0, report.remaining_lots)
        );
    }

    if (fill_lots > 0 && report.avg_fill_price_tick > 0) {
        child->total_cost_tick += fill_lots * report.avg_fill_price_tick;
        if (child->filled_qty_lots > 0) {
            child->avg_fill_price_tick =
                child->total_cost_tick / child->filled_qty_lots;
        }
    }

    refresh_plan_flags(&plan_state_);
}

FillTrackerState FillTracker::state() const noexcept {
    return state_;
}

const PlanFillState& FillTracker::plan_state() const noexcept {
    return plan_state_;
}

const ChildFillState* FillTracker::find_child(
    ChildOrderId order_id
) const noexcept {
    for (std::uint16_t i = 0; i < plan_state_.child_count; ++i) {
        if (plan_state_.children[i].order_id == order_id) {
            return &plan_state_.children[i];
        }
    }
    return nullptr;
}

}  // namespace trading_engine::execution
