#include "engine/execution/adapter/LiveExecutionAdapter.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace trading_engine::execution {

namespace {

ExecutionReport live_report(
    const OrderPlan& plan,
    const ChildOrder& order,
    ChildOrderStatus status,
    std::uint64_t now_ns,
    std::string venue_order_id = {},
    std::string reject_reason = {}
) {
    return {
        .plan_id = plan.plan_id,
        .child_order_id = order.order_id,
        .status = status,
        .venue_order_id = std::move(venue_order_id),
        .reject_reason = std::move(reject_reason),
        .filled_lots = 0,
        .remaining_lots = status == ChildOrderStatus::Acked
            ? order.quantity_lots
            : 0,
        .event_ts_ns = now_ns
    };
}

AdapterSubmitResult adapter_error(
    const OrderPlan& plan,
    PlanStatus status,
    std::uint64_t rejected,
    std::string error
) {
    return {
        .ok = false,
        .plan_id = plan.plan_id,
        .status = status,
        .child_orders_rejected = rejected,
        .code = AdapterResultCode::AdapterError,
        .error = std::move(error)
    };
}

}  // namespace

LiveExecutionAdapter::LiveExecutionAdapter(
    LiveExecutionConfig config,
    ILiveOrderSigner* signer,
    ILiveOrderTransport* transport
) : config_(std::move(config)),
    signer_(signer),
    transport_(transport) {
    pending_reports_.reserve(kMaxChildOrdersPerPlan);
}

AdapterSubmitResult LiveExecutionAdapter::disabled_submit(
    const OrderPlan& plan,
    const ExecutionContext& context
) const {
    std::string reason;
    if (!config_.enabled) {
        reason = "live execution adapter is disabled";
    } else if (context.config.mode != ExecutionMode::Live) {
        reason = "execution context is not live mode";
    } else if (
        config_.require_execution_enabled &&
        !context.config.execution_enabled
    ) {
        reason = "execution is not enabled";
    } else if (
        config_.require_context_live_enabled &&
        !context.config.live_enabled
    ) {
        reason = "live execution is not enabled";
    } else {
        reason = "live execution unavailable";
    }

    return {
        .ok = false,
        .plan_id = plan.plan_id,
        .status = PlanStatus::Rejected,
        .code = AdapterResultCode::LiveExecutionDisabled,
        .error = std::move(reason)
    };
}

bool LiveExecutionAdapter::live_context_enabled(
    const ExecutionContext& context
) const noexcept {
    if (!config_.enabled || context.config.mode != ExecutionMode::Live) {
        return false;
    }
    if (config_.require_execution_enabled &&
        !context.config.execution_enabled) {
        return false;
    }
    if (config_.require_context_live_enabled &&
        !context.config.live_enabled) {
        return false;
    }
    return true;
}

bool LiveExecutionAdapter::side_allowed(OrderSide side) const noexcept {
    return (side == OrderSide::Buy && config_.allow_buy_orders) ||
        (side == OrderSide::Sell && config_.allow_sell_orders);
}

bool LiveExecutionAdapter::child_notional_allowed(
    const ChildOrder& order
) const noexcept {
    if (config_.max_child_notional_tick <= 0) {
        return true;
    }
    if (order.quantity_lots <= 0 || order.limit_price_tick <= 0) {
        return false;
    }

    const auto notional =
        static_cast<__int128>(order.quantity_lots) *
        static_cast<__int128>(order.limit_price_tick);
    return notional <=
        static_cast<__int128>(config_.max_child_notional_tick);
}

LiveOrderRequest LiveExecutionAdapter::make_request(
    const OrderPlan& plan,
    const ChildOrder& order
) const {
    return {
        .parent_id = plan.plan_id,
        .child_id = order.order_id,
        .client_order_id = order.client_order_id,
        .market_id = order.market_id,
        .asset_id = order.asset_id,
        .side = order.side,
        .quantity_lots = order.quantity_lots,
        .price_tick = order.limit_price_tick,
        .created_ts_ns = plan.created_ts_ns,
        .expire_after_ns = plan.expire_after_ns,
        .order_type = config_.default_order_type,
        .post_only = false
    };
}

void LiveExecutionAdapter::push_rejection(
    const OrderPlan& plan,
    const ChildOrder& order,
    std::uint64_t now_ns,
    std::string reason
) {
    pending_reports_.push_back(live_report(
        plan,
        order,
        ChildOrderStatus::Rejected,
        now_ns,
        {},
        std::move(reason)
    ));
}

AdapterSubmitResult LiveExecutionAdapter::submit_plan(
    const OrderPlan& plan,
    const ExecutionContext& context
) {
    pending_reports_.clear();

    if (!live_context_enabled(context)) {
        return disabled_submit(plan, context);
    }
    if (signer_ == nullptr) {
        return adapter_error(
            plan,
            PlanStatus::Rejected,
            plan.order_count,
            "missing live order signer"
        );
    }
    if (transport_ == nullptr) {
        return adapter_error(
            plan,
            PlanStatus::Rejected,
            plan.order_count,
            "missing live order transport"
        );
    }
    if (plan.order_count == 0 ||
        plan.order_count >
            std::min<std::uint32_t>(
                config_.max_child_orders_per_plan,
                kMaxChildOrdersPerPlan
            )) {
        return adapter_error(
            plan,
            PlanStatus::Rejected,
            plan.order_count,
            "invalid live order_count"
        );
    }
    if (config_.reject_expired_plans &&
        plan.expire_after_ns != 0 &&
        context.now_ns >= plan.expire_after_ns) {
        for (std::uint16_t i = 0; i < plan.order_count; ++i) {
            push_rejection(plan, plan.orders[i], context.now_ns, "Expired");
        }
        return adapter_error(
            plan,
            PlanStatus::Rejected,
            plan.order_count,
            "live plan expired"
        );
    }

    std::uint64_t submitted = 0;
    std::uint64_t rejected = 0;
    std::vector<AcceptedLiveOrder> accepted;
    accepted.reserve(plan.order_count);

    for (std::uint16_t i = 0; i < plan.order_count; ++i) {
        const auto& order = plan.orders[i];
        if (!side_allowed(order.side)) {
            ++rejected;
            push_rejection(
                plan,
                order,
                context.now_ns,
                "live order side disabled"
            );
            continue;
        }
        if (!child_notional_allowed(order)) {
            ++rejected;
            push_rejection(
                plan,
                order,
                context.now_ns,
                "live child notional limit exceeded"
            );
            continue;
        }

        const auto signed_order = signer_->sign_order(make_request(plan, order));
        if (!signed_order.ok) {
            ++rejected;
            push_rejection(
                plan,
                order,
                context.now_ns,
                signed_order.error.empty() ? "live order signing failed"
                                           : signed_order.error
            );
            continue;
        }

        const auto transport_result =
            transport_->submit_order(signed_order.order.request_body_json);
        if (!transport_result.ok) {
            ++rejected;
            push_rejection(
                plan,
                order,
                context.now_ns,
                transport_result.error.empty() ? "live order submit failed"
                                               : transport_result.error
            );
            continue;
        }

        auto venue_order_id = transport_result.venue_order_id.empty()
            ? signed_order.order.venue_order_id_hint
            : transport_result.venue_order_id;
        if (venue_order_id.empty()) {
            ++rejected;
            push_rejection(
                plan,
                order,
                context.now_ns,
                "live order submit missing venue_order_id"
            );
            continue;
        }

        ++submitted;
        accepted.push_back({
            .child_order_id = order.order_id,
            .venue_order_id = venue_order_id
        });
        pending_reports_.push_back(live_report(
            plan,
            order,
            ChildOrderStatus::Acked,
            context.now_ns,
            std::move(venue_order_id)
        ));
    }

    if (!accepted.empty()) {
        accepted_orders_by_plan_[plan.plan_id] = std::move(accepted);
    }

    return {
        .ok = rejected == 0 && submitted > 0,
        .plan_id = plan.plan_id,
        .status = rejected == 0 && submitted > 0 ? PlanStatus::Acked
                                                 : PlanStatus::Rejected,
        .child_orders_submitted = submitted,
        .child_orders_rejected = rejected,
        .code = rejected == 0 && submitted > 0 ? AdapterResultCode::Ok
                                               : AdapterResultCode::AdapterError,
        .error = rejected == 0 ? std::string{} :
            "one or more live child orders failed"
    };
}

std::vector<ExecutionReport> LiveExecutionAdapter::poll_reports() {
    auto reports = std::move(pending_reports_);
    pending_reports_.clear();
    return reports;
}

AdapterCancelResult LiveExecutionAdapter::cancel_plan(
    std::uint64_t plan_id
) {
    if (!config_.enabled) {
        return {
            .ok = false,
            .plan_id = plan_id,
            .code = AdapterResultCode::LiveExecutionDisabled,
            .error = "live execution adapter is disabled"
        };
    }
    if (transport_ == nullptr) {
        return {
            .ok = false,
            .plan_id = plan_id,
            .code = AdapterResultCode::AdapterError,
            .error = "missing live order transport"
        };
    }

    const auto it = accepted_orders_by_plan_.find(plan_id);
    if (it == accepted_orders_by_plan_.end() || it->second.empty()) {
        return {
            .ok = false,
            .plan_id = plan_id,
            .code = AdapterResultCode::AdapterError,
            .error = "no live venue orders for plan"
        };
    }

    std::string first_error;
    std::uint64_t canceled = 0;
    for (const auto& order : it->second) {
        const auto result = transport_->cancel_order(order.venue_order_id);
        if (result.ok) {
            ++canceled;
            continue;
        }
        if (first_error.empty()) {
            first_error = result.error.empty() ? "live cancel failed"
                                               : result.error;
        }
    }

    if (canceled == it->second.size()) {
        accepted_orders_by_plan_.erase(it);
        return {
            .ok = true,
            .plan_id = plan_id,
            .code = AdapterResultCode::Ok
        };
    }

    return {
        .ok = false,
        .plan_id = plan_id,
        .code = AdapterResultCode::AdapterError,
        .error = first_error.empty() ? "live cancel failed" : first_error
    };
}

}  // namespace trading_engine::execution
