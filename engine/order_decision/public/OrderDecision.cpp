#include "engine/order_decision/public/OrderDecision.h"

#include "engine/order_decision/math/OrderDecisionMath.h"

namespace trading_engine::order_decision {

const char* to_string(OrderDecisionType type) noexcept {
    switch (type) {
        case OrderDecisionType::NoTrade:
            return "NoTrade";
        case OrderDecisionType::PaperOrderDecision:
            return "PaperOrderDecision";
        case OrderDecisionType::RejectNoDepth:
            return "RejectNoDepth";
        case OrderDecisionType::RejectLowEdge:
            return "RejectLowEdge";
        case OrderDecisionType::RejectInvalidBundle:
            return "RejectInvalidBundle";
        case OrderDecisionType::RejectUnsupportedSide:
            return "RejectUnsupportedSide";
        case OrderDecisionType::RejectRiskBudget:
            return "RejectRiskBudget";
        case OrderDecisionType::RejectPartialFillRisk:
            return "RejectPartialFillRisk";
        case OrderDecisionType::RejectExpiredIntent:
            return "RejectExpiredIntent";
        case OrderDecisionType::RejectPriceProtection:
            return "RejectPriceProtection";
        case OrderDecisionType::RejectInternalError:
            return "RejectInternalError";
    }
    return "Unknown";
}

OrderDecisionLite to_order_decision_lite(
    const OrderDecision& decision
) noexcept {
    OrderDecisionLite lite;
    lite.decision_id = decision.decision_id;
    lite.source_intent_id = decision.source_intent_id;
    lite.bundle_id = decision.bundle_id;
    lite.type = decision.type;
    lite.chosen_bundle_qty = decision.chosen_bundle_qty;
    lite.guaranteed_payout_tick = decision.guaranteed_payout_tick;
    lite.estimated_total_cost_tick = decision.estimated_total_cost_tick;
    lite.estimated_fee_tick = decision.estimated_fee_tick;
    lite.latency_buffer_tick = decision.latency_buffer_tick;
    lite.slippage_buffer_tick = decision.slippage_buffer_tick;
    lite.unit_edge_tick = decision.unit_edge_tick;
    lite.total_edge_tick = decision.total_edge_tick;
    lite.edge_bps = decision.edge_bps;
    lite.leg_count = decision.leg_count;
    lite.snapshot_version_hash = decision.snapshot_version_hash;
    lite.oracle_artifact_hash = decision.oracle_artifact_hash;
    lite.bundle_hash = decision.bundle_hash;
    lite.policy_hash = decision.policy_hash;
    lite.created_ts_ns = decision.created_ts_ns;
    lite.expires_at_ns = decision.expires_at_ns;
    lite.decision_hash = decision.decision_hash;
    for (std::uint16_t i = 0; i < decision.leg_count; ++i) {
        lite.legs[i].market_index = decision.legs[i].market_index;
        lite.legs[i].asset_index = decision.legs[i].asset_index;
        lite.legs[i].side = decision.legs[i].side;
        lite.legs[i].quantity_lots = decision.legs[i].quantity_lots;
        lite.legs[i].estimated_vwap_tick =
            decision.legs[i].estimated_vwap_tick;
        lite.legs[i].limit_price_tick = decision.legs[i].limit_price_tick;
        lite.legs[i].worst_price_tick = decision.legs[i].worst_price_tick;
        lite.legs[i].estimated_cost_tick =
            decision.legs[i].estimated_cost_tick;
    }
    return lite;
}

std::uint64_t compute_order_decision_hash(
    const OrderDecisionLite& decision
) noexcept {
    auto hash = kOrderDecisionFnvOffset;
    mix_u64(&hash, decision.source_intent_id);
    mix_u64(&hash, decision.bundle_id);
    mix_u64(&hash, static_cast<std::uint64_t>(decision.type));
    mix_i64(&hash, decision.chosen_bundle_qty);
    mix_i64(&hash, decision.guaranteed_payout_tick);
    mix_i64(&hash, decision.estimated_total_cost_tick);
    mix_i64(&hash, decision.estimated_fee_tick);
    mix_i64(&hash, decision.latency_buffer_tick);
    mix_i64(&hash, decision.slippage_buffer_tick);
    mix_i64(&hash, decision.unit_edge_tick);
    mix_i64(&hash, decision.total_edge_tick);
    mix_i64(&hash, decision.edge_bps);
    mix_u64(&hash, decision.snapshot_version_hash);
    mix_u64(&hash, decision.oracle_artifact_hash);
    mix_u64(&hash, decision.bundle_hash);
    mix_u64(&hash, decision.policy_hash);
    mix_u64(&hash, decision.created_ts_ns);
    mix_u64(&hash, decision.expires_at_ns);
    mix_u64(&hash, decision.leg_count);
    for (std::uint16_t i = 0; i < decision.leg_count; ++i) {
        const auto& leg = decision.legs[i];
        mix_u64(&hash, leg.market_index);
        mix_u64(&hash, leg.asset_index);
        mix_u64(&hash, static_cast<std::uint64_t>(leg.side));
        mix_i64(&hash, leg.quantity_lots);
        mix_i64(&hash, leg.estimated_vwap_tick);
        mix_i64(&hash, leg.limit_price_tick);
        mix_i64(&hash, leg.worst_price_tick);
        mix_i64(&hash, leg.estimated_cost_tick);
    }
    return hash == 0 ? 1 : hash;
}

std::uint64_t compute_order_decision_hash(
    const OrderDecision& decision
) noexcept {
    return compute_order_decision_hash(to_order_decision_lite(decision));
}

}  // namespace trading_engine::order_decision
