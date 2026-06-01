#pragma once

#include "engine/order_decision/core/OrderDecisionCache.h"
#include "engine/order_decision/public/OrderDecisionConfig.h"
#include "engine/order_decision/public/OrderDecisionResult.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/view/MarketDepthView.h"
#include "oracle/public/CandidateBundle.h"

#include <span>

namespace trading_engine::order_decision {

class OrderDecisionEngine {
public:
    explicit OrderDecisionEngine(OrderDecisionConfig config = {});

    [[nodiscard]] OrderDecisionResult decide(
        const signal::OpportunityIntent& intent,
        const oracle::CandidateBundle& bundle,
        std::span<const state::MarketDepthView> depth_views,
        const risk::RiskPolicySnapshot& policy,
        std::uint64_t now_ns
    ) const;

private:
    OrderDecisionConfig config_;
    mutable OrderDecisionCache cache_;
};

}  // namespace trading_engine::order_decision
