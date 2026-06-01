#pragma once

#include "engine/order_decision/public/OrderDecision.h"
#include "engine/order_decision/public/OrderDecisionEvalStats.h"
#include "engine/order_decision/public/OrderDecisionStageTimings.h"

#include <string>

namespace trading_engine::order_decision {

struct OrderDecisionResult {
    bool ok = false;
    OrderDecisionLite decision;
    OrderDecisionType reject_reason = OrderDecisionType::NoTrade;
    std::string error;
    OrderDecisionStageTimings timings;
    OrderDecisionEvalStats eval_stats;
};

}  // namespace trading_engine::order_decision
