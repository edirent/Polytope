#include "engine/execution/plan/PlanValidator.h"

#include <algorithm>
#include <unordered_set>

namespace trading_engine::execution {

namespace {

ValidationResult validate_impl(
    const OrderPlan& plan,
    const ExecutionConfig& config,
    bool require_reservation,
    const PlanValidator& validator
) {
    if (plan.plan_id == 0) {
        return {.ok = false, .error = "missing plan_id"};
    }
    if (plan.source_intent_id == 0) {
        return {.ok = false, .error = "missing source_intent_id"};
    }
    if (plan.approved_intent_id == 0) {
        return {.ok = false, .error = "missing approved_intent_id"};
    }
    if (require_reservation && plan.reservation_id == 0) {
        return {.ok = false, .error = "missing reservation_id"};
    }
    const auto max_orders = std::min<std::uint32_t>(
        config.max_child_orders_per_plan,
        kMaxChildOrdersPerPlan
    );
    if (max_orders == 0 ||
        plan.order_count == 0 ||
        plan.order_count > max_orders) {
        return {.ok = false, .error = "invalid order_count"};
    }
    if (plan.max_total_cost_tick <= 0) {
        return {.ok = false, .error = "missing max_total_cost_tick"};
    }
    if (plan.max_slippage_tick < 0) {
        return {.ok = false, .error = "invalid max_slippage_tick"};
    }
    if (plan.idempotency_key.empty()) {
        return {.ok = false, .error = "missing idempotency_key"};
    }
    if (plan.expire_after_ns <= plan.created_ts_ns) {
        return {.ok = false, .error = "invalid expiration"};
    }
    if (config.max_order_age_ns > 0 &&
        plan.expire_after_ns - plan.created_ts_ns >
            static_cast<std::uint64_t>(config.max_order_age_ns)) {
        return {.ok = false, .error = "ttl exceeds max_order_age_ns"};
    }

    std::unordered_set<std::string> client_order_ids;
    for (std::uint16_t i = 0; i < plan.order_count; ++i) {
        const auto result = validator.validate_child_order(plan.orders[i]);
        if (!result.ok) {
            return result;
        }
        if (plan.orders[i].plan_id != plan.plan_id) {
            return {.ok = false, .error = "child plan_id mismatch"};
        }
        const auto [_, inserted] =
            client_order_ids.insert(plan.orders[i].client_order_id);
        if (!inserted) {
            return {.ok = false, .error = "duplicate client_order_id"};
        }
    }
    return {.ok = true};
}

}  // namespace

ValidationResult PlanValidator::validate(
    const OrderPlan& plan,
    const ExecutionConfig& config
) const {
    return validate_impl(plan, config, true, *this);
}

PlanValidationResult PlanValidator::validate(
    const OrderPlan& plan,
    bool require_reservation
) const {
    ExecutionConfig config;
    config.max_order_age_ns = 0;
    return validate_impl(plan, config, require_reservation, *this);
}

PlanValidationResult PlanValidator::validate_child_order(
    const ChildOrder& order
) const {
    if (order.order_id == 0) {
        return {.ok = false, .error = "missing order_id"};
    }
    if (order.plan_id == 0) {
        return {.ok = false, .error = "missing plan_id"};
    }
    if (order.client_order_id.empty()) {
        return {.ok = false, .error = "missing client_order_id"};
    }
    if (order.market_id.empty()) {
        return {.ok = false, .error = "missing market_id"};
    }
    if (order.asset_id.empty()) {
        return {.ok = false, .error = "missing asset_id"};
    }
    if (order.quantity_lots <= 0) {
        return {.ok = false, .error = "invalid quantity_lots"};
    }
    if (order.limit_price_tick <= 0) {
        return {.ok = false, .error = "invalid limit_price_tick"};
    }
    if (order.worst_allowed_price_tick <= 0) {
        return {.ok = false, .error = "invalid worst_allowed_price_tick"};
    }
    return {.ok = true};
}

}  // namespace trading_engine::execution
