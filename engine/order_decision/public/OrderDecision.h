#pragma once

#include "engine/order_decision/public/OrderDecisionTypes.h"

#include <array>
#include <cstdint>
#include <string>

namespace trading_engine::order_decision {

struct OrderDecisionLeg {
    std::string market_id;
    std::string asset_id;

    std::uint32_t market_index = 0;
    std::uint32_t asset_index = 0;

    Side side = Side::Buy;

    std::int64_t quantity_lots = 0;

    std::int64_t estimated_vwap_tick = 0;
    std::int64_t limit_price_tick = 0;
    std::int64_t worst_price_tick = 0;

    std::int64_t estimated_cost_tick = 0;
};

struct OrderDecisionLegLite {
    std::uint32_t market_index = 0;
    std::uint32_t asset_index = 0;

    Side side = Side::Buy;

    std::int64_t quantity_lots = 0;

    std::int64_t estimated_vwap_tick = 0;
    std::int64_t limit_price_tick = 0;
    std::int64_t worst_price_tick = 0;

    std::int64_t estimated_cost_tick = 0;
};

struct OrderDecisionLite {
    std::uint64_t decision_id = 0;

    std::uint64_t source_intent_id = 0;
    std::uint64_t bundle_id = 0;

    OrderDecisionType type = OrderDecisionType::NoTrade;

    std::int64_t chosen_bundle_qty = 0;

    std::int64_t guaranteed_payout_tick = 0;
    std::int64_t estimated_total_cost_tick = 0;
    std::int64_t estimated_fee_tick = 0;
    std::int64_t latency_buffer_tick = 0;
    std::int64_t slippage_buffer_tick = 0;

    std::int64_t unit_edge_tick = 0;
    std::int64_t total_edge_tick = 0;
    std::int64_t edge_bps = 0;

    std::uint16_t leg_count = 0;
    std::array<OrderDecisionLegLite, kMaxOrderDecisionLegs> legs{};

    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t oracle_artifact_hash = 0;
    std::uint64_t bundle_hash = 0;
    std::uint64_t policy_hash = 0;

    std::uint64_t created_ts_ns = 0;
    std::uint64_t expires_at_ns = 0;

    std::uint64_t decision_hash = 0;
};

struct OrderDecision {
    std::uint64_t decision_id = 0;

    std::uint64_t source_intent_id = 0;
    std::uint64_t bundle_id = 0;

    OrderDecisionType type = OrderDecisionType::NoTrade;

    std::int64_t chosen_bundle_qty = 0;

    std::int64_t guaranteed_payout_tick = 0;
    std::int64_t estimated_total_cost_tick = 0;
    std::int64_t estimated_fee_tick = 0;
    std::int64_t latency_buffer_tick = 0;
    std::int64_t slippage_buffer_tick = 0;

    std::int64_t unit_edge_tick = 0;
    std::int64_t total_edge_tick = 0;
    std::int64_t edge_bps = 0;

    std::uint16_t leg_count = 0;
    std::array<OrderDecisionLeg, kMaxOrderDecisionLegs> legs{};

    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t oracle_artifact_hash = 0;
    std::uint64_t bundle_hash = 0;
    std::uint64_t policy_hash = 0;

    std::uint64_t created_ts_ns = 0;
    std::uint64_t expires_at_ns = 0;

    std::uint64_t decision_hash = 0;

    std::string reason;
};

[[nodiscard]] OrderDecisionLite to_order_decision_lite(
    const OrderDecision& decision
) noexcept;

[[nodiscard]] std::uint64_t compute_order_decision_hash(
    const OrderDecisionLite& decision
) noexcept;

[[nodiscard]] std::uint64_t compute_order_decision_hash(
    const OrderDecision& decision
) noexcept;

}  // namespace trading_engine::order_decision
