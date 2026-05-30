#include "engine/execution/plan/ExecutionPlanner.h"

#include "engine/execution/plan/OrderBuilder.h"
#include "engine/execution/plan/PlanValidator.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace trading_engine::execution {

namespace {

inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= (value >> shift) & 0xffU;
        *hash *= kFnvPrime;
    }
}

void mix_string(std::uint64_t* hash, const std::string& value) noexcept {
    for (const unsigned char ch : value) {
        *hash ^= ch;
        *hash *= kFnvPrime;
    }
}

std::uint64_t make_plan_id(
    const ApprovedIntentEnvelope& envelope
) noexcept {
    auto hash = kFnvOffset;
    mix_u64(&hash, envelope.source_intent.intent_id);
    mix_u64(&hash, envelope.approval.decision_id);
    mix_u64(&hash, envelope.approval.reservation_id);
    mix_u64(&hash, envelope.approval.bundle_id);
    mix_string(&hash, envelope.approval.idempotency_key);
    return hash == 0 ? 1 : hash;
}

std::string make_client_order_id(
    std::uint64_t plan_id,
    std::uint16_t order_index,
    const std::string& idempotency_key
) {
    auto hash = kFnvOffset;
    mix_u64(&hash, plan_id);
    mix_u64(&hash, order_index);
    mix_string(&hash, idempotency_key);
    return std::to_string(hash);
}

PlanResult reject(std::string error) {
    return {.ok = false, .error = std::move(error)};
}

bool multiply_would_overflow(std::int64_t left, std::int64_t right) noexcept {
    if (left <= 0 || right <= 0) {
        return false;
    }
    return left > (std::numeric_limits<std::int64_t>::max() / right);
}

}  // namespace

PlanResult ExecutionPlanner::build_plan(
    const ApprovedIntentEnvelope& envelope,
    std::uint64_t now_ns,
    const ExecutionConfig& config
) const {
    const auto& intent = envelope.source_intent;
    const auto& approval = envelope.approval;

    if (approval.decision_id == 0) {
        return reject("missing decision_id");
    }
    if (approval.reservation_id == 0) {
        return reject("missing reservation_id");
    }
    if (intent.status != signal::IntentStatus::PaperOpportunity) {
        return reject("source intent is not PaperOpportunity");
    }
    if (intent.expires_at_ns <= now_ns) {
        return reject("source intent expired");
    }
    if (intent.idempotency_key != approval.idempotency_key) {
        return reject("idempotency_key mismatch");
    }
    if (intent.bundle_id != approval.bundle_id) {
        return reject("bundle_id mismatch");
    }
    if (approval.approved_bundle_qty <= 0) {
        return reject("invalid approved_bundle_qty");
    }
    if (intent.leg_count == 0) {
        return reject("source intent has no legs");
    }

    const auto max_orders = std::min<std::uint32_t>(
        config.max_child_orders_per_plan,
        kMaxChildOrdersPerPlan
    );
    if (intent.leg_count > max_orders) {
        return reject("too many child orders");
    }

    OrderPlan plan;
    plan.plan_id = make_plan_id(envelope);
    plan.source_intent_id = intent.intent_id;
    plan.approved_intent_id = approval.decision_id;
    plan.reservation_id = approval.reservation_id;
    plan.bundle_id = intent.bundle_id;
    plan.order_count = intent.leg_count;
    plan.max_total_cost_tick = intent.estimated_cost_tick;
    plan.min_expected_edge_tick = intent.total_edge_tick != 0
        ? intent.total_edge_tick
        : intent.estimated_edge_tick;
    plan.max_slippage_tick = intent.slippage_buffer_tick;
    plan.created_ts_ns = now_ns;
    plan.expire_after_ns = intent.expires_at_ns;
    plan.idempotency_key = intent.idempotency_key;

    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto& leg = intent.legs[i];
        if (leg.side != signal::Side::Buy) {
            return reject("UnsupportedSide");
        }
        if (leg.quantity_lots <= 0) {
            return reject("invalid leg quantity");
        }
        if (multiply_would_overflow(approval.approved_bundle_qty, leg.quantity_lots)) {
            return reject("child order quantity overflow");
        }

        auto& order = plan.orders[i];
        order.order_id = static_cast<ChildOrderId>(i + 1);
        order.plan_id = plan.plan_id;
        order.client_order_id =
            make_client_order_id(plan.plan_id, i, plan.idempotency_key);
        order.market_id = leg.market_id;
        order.asset_id = leg.asset_id;
        order.side = OrderSide::Buy;
        order.quantity_lots =
            approval.approved_bundle_qty * leg.quantity_lots;
        order.limit_price_tick = leg.worst_price_tick;
        order.estimated_vwap_tick = leg.estimated_vwap_tick;
        order.worst_allowed_price_tick = leg.worst_price_tick;
    }

    const PlanValidator validator;
    const auto validation = validator.validate(plan);
    if (!validation.ok) {
        return reject(validation.error);
    }

    return {.ok = true, .plan = std::move(plan)};
}

OrderPlan ExecutionPlanner::plan(
    const risk::ApprovedIntent& approved_intent,
    const ExecutionContext& context
) const {
    OrderBuilder builder;
    return builder.build(approved_intent, context);
}

}  // namespace trading_engine::execution
