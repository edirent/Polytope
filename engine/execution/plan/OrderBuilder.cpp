#include "engine/execution/plan/OrderBuilder.h"

#include <algorithm>
#include <charconv>
#include <string>
#include <system_error>

namespace trading_engine::execution {

namespace {

std::uint64_t reservation_id_from_string(const std::string& value) noexcept {
    if (value.empty()) {
        return 0;
    }

    std::uint64_t parsed = 0;
    const auto* first = value.data();
    const auto* last = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(first, last, parsed);
    if (ec == std::errc{} && ptr == last) {
        return parsed;
    }

    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

}  // namespace

OrderPlan OrderBuilder::build(
    const risk::ApprovedIntent& approved_intent,
    const ExecutionContext& context
) const {
    OrderPlan plan;
    plan.plan_id = approved_intent.intent.intent_id;
    plan.source_intent_id = approved_intent.intent.intent_id;
    plan.approved_intent_id = approved_intent.intent.intent_id;
    plan.reservation_id =
        reservation_id_from_string(approved_intent.reservation_id);
    plan.bundle_id = approved_intent.intent.bundle_id;
    plan.created_ts_ns = context.now_ns;
    plan.expire_after_ns = approved_intent.expires_at_ns;
    plan.max_total_cost_tick = approved_intent.intent.estimated_cost_tick;
    plan.min_expected_edge_tick = approved_intent.intent.total_edge_tick;
    plan.max_slippage_tick = approved_intent.intent.slippage_buffer_tick;
    plan.idempotency_key = approved_intent.intent.idempotency_key;

    const auto count = std::min<std::uint16_t>(
        approved_intent.intent.leg_count,
        kMaxChildOrdersPerPlan
    );
    plan.order_count = count;
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto& leg = approved_intent.intent.legs[i];
        auto& child = plan.orders[i];
        child.order_id = static_cast<ChildOrderId>(i + 1);
        child.plan_id = plan.plan_id;
        child.client_order_id =
            plan.idempotency_key + "-" + std::to_string(i + 1);
        child.market_id = leg.market_id;
        child.asset_id = leg.asset_id;
        child.side = leg.side == trading_engine::oracle::Side::Buy
            ? OrderSide::Buy
            : OrderSide::Sell;
        child.quantity_lots = leg.quantity_lots;
        child.limit_price_tick = leg.worst_price_tick;
        child.estimated_vwap_tick = leg.estimated_vwap_tick;
        child.worst_allowed_price_tick = leg.worst_price_tick;
    }
    return plan;
}

}  // namespace trading_engine::execution
