#include "engine/execution/adapter/PaperExecutionAdapter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace trading_engine::execution {

namespace {

struct FillSimulation {
    bool snapshot_found = false;
    bool full = false;
    bool partial = false;

    std::int64_t filled_lots = 0;
    std::int64_t remaining_lots = 0;
    std::int64_t avg_fill_price_tick = 0;

    std::string reject_reason;
};

const state::MarketStateSnapshot* find_snapshot(
    const std::vector<state::MarketStateSnapshot>& snapshots,
    const std::string& asset_id
) {
    const auto it = std::find_if(
        snapshots.begin(),
        snapshots.end(),
        [&asset_id](const state::MarketStateSnapshot& snapshot) {
            return snapshot.entity_id == asset_id;
        }
    );
    return it == snapshots.end() ? nullptr : &*it;
}

std::int64_t size_to_lots(double size) {
    if (size <= 0.0) {
        return 0;
    }
    return static_cast<std::int64_t>(std::floor(size));
}

FillSimulation simulate_buy(
    const ChildOrder& order,
    const std::vector<state::MarketStateSnapshot>& snapshots
) {
    FillSimulation result;
    result.remaining_lots = order.quantity_lots;

    const auto* snapshot = find_snapshot(snapshots, order.asset_id);
    if (snapshot == nullptr) {
        result.reject_reason = "MissingSnapshot";
        return result;
    }
    result.snapshot_found = true;

    if (!snapshot->usable_for_depth || snapshot->ask_count == 0) {
        result.reject_reason = "InsufficientDepth";
        return result;
    }

    std::int64_t remaining = order.quantity_lots;
    std::int64_t filled = 0;
    std::int64_t notional = 0;

    for (std::uint32_t i = 0; i < snapshot->ask_count && remaining > 0; ++i) {
        const auto& level = snapshot->asks[i];
        if (level.price_tick <= 0 ||
            level.price_tick > order.limit_price_tick) {
            continue;
        }

        const auto available = size_to_lots(level.size);
        if (available <= 0) {
            continue;
        }

        const auto take = std::min(remaining, available);
        filled += take;
        remaining -= take;
        notional += take * level.price_tick;
    }

    result.filled_lots = filled;
    result.remaining_lots = remaining;
    result.full = filled == order.quantity_lots;
    result.partial = filled > 0 && !result.full;
    if (filled > 0) {
        result.avg_fill_price_tick = notional / filled;
    }
    if (!result.full) {
        result.reject_reason = "InsufficientDepth";
    }
    return result;
}

ExecutionReport make_report(
    const OrderPlan& plan,
    const ChildOrder& order,
    ChildOrderStatus status,
    const FillSimulation& fill,
    std::uint64_t now_ns,
    std::string reject_reason = {}
) {
    return {
        .plan_id = plan.plan_id,
        .child_order_id = order.order_id,
        .status = status,
        .reject_reason = reject_reason.empty() ? fill.reject_reason
                                               : std::move(reject_reason),
        .filled_lots = fill.filled_lots,
        .remaining_lots = fill.remaining_lots,
        .avg_fill_price_tick = fill.avg_fill_price_tick,
        .event_ts_ns = now_ns
    };
}

}  // namespace

AdapterSubmitResult PaperExecutionAdapter::submit_plan(
    const OrderPlan& plan,
    const ExecutionContext& context
) {
    pending_reports_.clear();

    if (plan.expire_after_ns != 0 && context.now_ns >= plan.expire_after_ns) {
        for (std::uint16_t i = 0; i < plan.order_count; ++i) {
            FillSimulation fill;
            fill.remaining_lots = plan.orders[i].quantity_lots;
            pending_reports_.push_back(make_report(
                plan,
                plan.orders[i],
                ChildOrderStatus::Rejected,
                fill,
                context.now_ns,
                "Expired"
            ));
        }
        return {
            .ok = false,
            .plan_id = plan.plan_id,
            .status = PlanStatus::Rejected,
            .child_orders_rejected = plan.order_count,
            .code = AdapterResultCode::AdapterError,
            .error = "paper plan expired"
        };
    }

    if (context.config.paper_mode == PaperExecutionMode::PaperAtomic) {
        std::vector<FillSimulation> fills;
        fills.reserve(plan.order_count);
        for (std::uint16_t i = 0; i < plan.order_count; ++i) {
            const auto& order = plan.orders[i];
            if (order.side != OrderSide::Buy) {
                FillSimulation fill;
                fill.remaining_lots = order.quantity_lots;
                fill.reject_reason = "UnsupportedSide";
                fills.push_back(std::move(fill));
                continue;
            }
            fills.push_back(simulate_buy(order, context.snapshots));
        }

        const auto all_filled = std::all_of(
            fills.begin(),
            fills.end(),
            [](const FillSimulation& fill) { return fill.full; }
        );
        if (!all_filled) {
            for (std::uint16_t i = 0; i < plan.order_count; ++i) {
                FillSimulation no_fill = fills[i];
                no_fill.filled_lots = 0;
                no_fill.remaining_lots = plan.orders[i].quantity_lots;
                no_fill.avg_fill_price_tick = 0;
                pending_reports_.push_back(make_report(
                    plan,
                    plan.orders[i],
                    ChildOrderStatus::Rejected,
                    no_fill,
                    context.now_ns
                ));
            }
            return {
                .ok = false,
                .plan_id = plan.plan_id,
                .status = PlanStatus::Rejected,
                .child_orders_rejected = plan.order_count,
                .code = AdapterResultCode::AdapterError,
                .error = "paper atomic fill failed"
            };
        }

        for (std::uint16_t i = 0; i < plan.order_count; ++i) {
            pending_reports_.push_back(make_report(
                plan,
                plan.orders[i],
                ChildOrderStatus::Filled,
                fills[i],
                context.now_ns
            ));
        }
        return {
            .ok = true,
            .plan_id = plan.plan_id,
            .status = PlanStatus::Filled,
            .child_orders_submitted = plan.order_count,
            .code = AdapterResultCode::Ok
        };
    }

    std::uint64_t filled_count = 0;
    std::uint64_t partial_count = 0;
    std::uint64_t rejected_count = 0;
    for (std::uint16_t i = 0; i < plan.order_count; ++i) {
        const auto& order = plan.orders[i];
        auto fill = order.side == OrderSide::Buy
            ? simulate_buy(order, context.snapshots)
            : FillSimulation{
                  .remaining_lots = order.quantity_lots,
                  .reject_reason = "UnsupportedSide"
              };

        if (fill.full) {
            ++filled_count;
            pending_reports_.push_back(make_report(
                plan,
                order,
                ChildOrderStatus::Filled,
                fill,
                context.now_ns
            ));
        } else if (fill.partial && context.config.allow_partial_fill_paper) {
            ++partial_count;
            pending_reports_.push_back(make_report(
                plan,
                order,
                ChildOrderStatus::PartiallyFilled,
                fill,
                context.now_ns
            ));
        } else {
            ++rejected_count;
            fill.filled_lots = 0;
            fill.remaining_lots = order.quantity_lots;
            fill.avg_fill_price_tick = 0;
            pending_reports_.push_back(make_report(
                plan,
                order,
                ChildOrderStatus::Rejected,
                fill,
                context.now_ns
            ));
        }
    }

    return {
        .ok = rejected_count == 0,
        .plan_id = plan.plan_id,
        .status = partial_count > 0 ? PlanStatus::PartiallyFilled :
            (rejected_count == 0 ? PlanStatus::Filled : PlanStatus::Rejected),
        .child_orders_submitted = filled_count + partial_count,
        .child_orders_rejected = rejected_count,
        .code = rejected_count == 0 ? AdapterResultCode::Ok :
                                      AdapterResultCode::AdapterError,
        .error = rejected_count == 0 ? std::string{} :
                                       std::string{"paper sequential fill failed"}
    };
}

std::vector<ExecutionReport> PaperExecutionAdapter::poll_reports() {
    auto reports = std::move(pending_reports_);
    pending_reports_.clear();
    return reports;
}

AdapterCancelResult PaperExecutionAdapter::cancel_plan(
    std::uint64_t plan_id
) {
    return {
        .ok = true,
        .plan_id = plan_id,
        .code = AdapterResultCode::Ok
    };
}

}  // namespace trading_engine::execution
