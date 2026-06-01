#pragma once

#include "engine/order_decision/math/CostCurve.h"
#include "engine/state/view/MarketDepthView.h"
#include "oracle/public/CandidateBundle.h"

#include <string>

namespace trading_engine::order_decision {

struct CostCurveBuildInput {
    const oracle::BundleLeg* leg = nullptr;
    const state::MarketDepthView* depth = nullptr;
    std::uint32_t asset_index = 0;
};

struct CostCurveBuildResult {
    bool ok = false;
    CostCurve curve;
    OrderDecisionType reject_reason = OrderDecisionType::NoTrade;
    std::string error;
};

class CostCurveBuilder {
public:
    [[nodiscard]] CostCurveBuildResult build_buy_curve(
        const CostCurveBuildInput& input
    ) const;
};

}  // namespace trading_engine::order_decision
