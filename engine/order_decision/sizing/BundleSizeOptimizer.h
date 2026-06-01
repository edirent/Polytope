#pragma once

#include "engine/order_decision/public/OrderDecision.h"
#include "engine/order_decision/public/OrderDecisionConfig.h"
#include "engine/order_decision/public/OrderDecisionEvalStats.h"
#include "engine/order_decision/public/OrderDecisionStageTimings.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/view/MarketDepthView.h"
#include "oracle/public/CandidateBundle.h"

#include <array>
#include <span>

namespace trading_engine::order_decision {

class OrderDecisionCache;

struct BundleSizeInput {
    const signal::OpportunityIntent* intent = nullptr;
    const oracle::CandidateBundle* bundle = nullptr;

    std::span<const state::MarketDepthView> depth_views;

    const risk::RiskPolicySnapshot* policy = nullptr;
    OrderDecisionConfig config;
    OrderDecisionCache* cache = nullptr;
};

struct BundleSizeResult {
    bool ok = false;

    std::int64_t best_bundle_qty = 0;

    std::int64_t guaranteed_payout_tick = 0;
    std::int64_t total_cost_tick = 0;
    std::int64_t unit_edge_tick = 0;
    std::int64_t total_edge_tick = 0;
    std::int64_t edge_bps = 0;

    std::uint16_t leg_count = 0;
    std::array<OrderDecisionLegLite, kMaxOrderDecisionLegs> legs{};

    OrderDecisionType reject_reason = OrderDecisionType::NoTrade;
    OrderDecisionStageTimings timings;
    OrderDecisionEvalStats eval_stats;
};

class BundleSizeOptimizer {
public:
    [[nodiscard]] BundleSizeResult optimize(
        const BundleSizeInput& input
    ) const;
};

}  // namespace trading_engine::order_decision
