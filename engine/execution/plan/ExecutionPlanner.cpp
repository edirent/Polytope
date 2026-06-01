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

std::uint64_t make_plan_id(
    const ApprovedOrderDecisionEnvelope& envelope
) noexcept {
    auto hash = kFnvOffset;
    mix_u64(&hash, envelope.source_intent_id);
    mix_u64(&hash, envelope.decision_hash);
    mix_u64(&hash, envelope.approval_hash);
    mix_u64(&hash, envelope.approved.decision.decision_id);
    mix_u64(&hash, envelope.approved.reservation_id_hash);
    mix_u64(&hash, envelope.bundle_id);
    mix_string(&hash, envelope.approved.intent.idempotency_key);
    return hash == 0 ? 1 : hash;
}

std::uint64_t make_numeric_plan_id(
    const ApprovedOrderDecisionEnvelope& envelope
) noexcept {
    auto hash = kFnvOffset;
    mix_u64(&hash, envelope.source_intent_id);
    mix_u64(&hash, envelope.decision_hash);
    mix_u64(&hash, envelope.approval_hash);
    mix_u64(&hash, envelope.approved.decision.decision_id);
    mix_u64(&hash, envelope.approved.reservation_id_hash);
    mix_u64(&hash, envelope.approved.intent.idempotency_hash);
    mix_u64(&hash, envelope.bundle_id);
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
        order.asset_index = leg.asset_index;
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

PlanResult ExecutionPlanner::build_plan(
    const ApprovedOrderDecisionEnvelope& envelope,
    std::uint64_t now_ns,
    const ExecutionConfig& config
) const {
    const auto& approved = envelope.approved;
    const auto& intent = approved.intent;
    const auto& decision = envelope.decision;

    if (!approved.valid()) {
        return reject("approved intent is invalid");
    }
    if (approved.decision.decision_id == 0) {
        return reject("missing decision_id");
    }
    if (approved.reservation_id_hash == 0 && approved.reservation_id.empty()) {
        return reject("missing reservation_id");
    }
    if (decision.type !=
        order_decision::OrderDecisionType::PaperOrderDecision) {
        return reject("order decision is not PaperOrderDecision");
    }
    if (decision.expires_at_ns <= now_ns) {
        return reject("order decision expired");
    }
    if (intent.intent_id != decision.source_intent_id) {
        return reject("decision intent mismatch");
    }
    if (approved.decision.intent_id != 0 &&
        approved.decision.intent_id != decision.source_intent_id) {
        return reject("decision approval mismatch");
    }
    if (approved.decision.idempotency_hash != 0 &&
        intent.idempotency_hash != 0 &&
        approved.decision.idempotency_hash != intent.idempotency_hash) {
        return reject("decision approval mismatch");
    }
    if (intent.bundle_id != decision.bundle_id ||
        approved.decision.bundle_id != decision.bundle_id ||
        envelope.bundle_id != decision.bundle_id) {
        return reject("bundle_id mismatch");
    }
    if (envelope.source_intent_id != 0 &&
        envelope.source_intent_id != decision.source_intent_id) {
        return reject("envelope source_intent_id mismatch");
    }
    if (envelope.decision_hash != 0 &&
        envelope.decision_hash != decision.decision_hash) {
        return reject("envelope decision_hash mismatch");
    }
    if (!config.hot_path_trust_order_decision_hash) {
        if (order_decision::compute_order_decision_hash(decision) !=
            decision.decision_hash) {
            return reject("decision hash mismatch");
        }
    }
    if (envelope.approval_hash != 0 &&
        !config.hot_path_trust_approval_hash &&
        envelope.approval_hash !=
            order_decision::compute_approved_intent_hash(approved)) {
        return reject("approval hash mismatch");
    }
    if (decision.leg_count == 0) {
        return reject("order decision has no legs");
    }

    const auto max_orders = std::min<std::uint32_t>(
        config.max_child_orders_per_plan,
        kMaxChildOrdersPerPlan
    );
    if (decision.leg_count > max_orders) {
        return reject("too many child orders");
    }

    OrderPlan plan;
    plan.plan_id = config.hot_path_numeric_plan_id
        ? make_numeric_plan_id(envelope)
        : make_plan_id(envelope);
    plan.source_intent_id = decision.source_intent_id;
    plan.approved_intent_id = approved.decision.decision_id;
    plan.reservation_id = approved.reservation_id_hash;
    plan.bundle_id = decision.bundle_id;
    plan.order_count = decision.leg_count;
    plan.max_total_cost_tick = decision.estimated_total_cost_tick;
    plan.min_expected_edge_tick = decision.total_edge_tick;
    plan.max_slippage_tick = decision.slippage_buffer_tick;
    plan.created_ts_ns = now_ns;
    plan.expire_after_ns = decision.expires_at_ns;
    if (!config.hot_path_skip_order_strings) {
        plan.idempotency_key = intent.idempotency_key;
    }

    for (std::uint16_t i = 0; i < decision.leg_count; ++i) {
        const auto& leg = decision.legs[i];
        if (leg.side != signal::Side::Buy) {
            return reject("UnsupportedSide");
        }
        if (leg.quantity_lots <= 0 || leg.limit_price_tick <= 0) {
            return reject("invalid decision leg");
        }

        auto& order = plan.orders[i];
        order.order_id = static_cast<ChildOrderId>(i + 1);
        order.plan_id = plan.plan_id;
        order.market_index = leg.market_index;
        order.asset_index = leg.asset_index;
        if (!config.hot_path_skip_order_strings) {
            order.client_order_id =
                make_client_order_id(plan.plan_id, i, plan.idempotency_key);
        }
        if (!config.hot_path_skip_order_strings && intent.leg_count > i) {
            order.market_id = intent.legs[i].market_id;
            order.asset_id = intent.legs[i].asset_id;
        }
        order.side = OrderSide::Buy;
        order.quantity_lots = leg.quantity_lots;
        order.limit_price_tick = leg.limit_price_tick;
        order.estimated_vwap_tick = leg.estimated_vwap_tick;
        order.worst_allowed_price_tick = leg.limit_price_tick;
    }

    if (!config.hot_path_skip_plan_validation) {
        const PlanValidator validator;
        const auto validation = validator.validate(plan);
        if (!validation.ok) {
            return reject(validation.error);
        }
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
