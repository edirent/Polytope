#include "engine/order_decision/core/OrderDecisionEngine.h"

#include "engine/order_decision/core/FastFixedShapeOrderDecisionImpl.h"
#include "engine/order_decision/core/GenericOrderDecisionImpl.h"

namespace trading_engine::order_decision {

OrderDecisionEngine::OrderDecisionEngine(OrderDecisionConfig config)
    : config_(config) {}

OrderDecisionResult OrderDecisionEngine::decide(
    const signal::OpportunityIntent& intent,
    const oracle::CandidateBundle& bundle,
    std::span<const state::MarketDepthView> depth_views,
    const risk::RiskPolicySnapshot& policy,
    std::uint64_t now_ns
) const {
    if (config_.impl_mode == OrderDecisionImplMode::FastFixedShape) {
        const auto fast_result = FastFixedShapeOrderDecisionImpl{}.decide(
            intent,
            bundle,
            depth_views,
            policy,
            now_ns,
            config_,
            &cache_
        );
        if (fast_result.ok || !config_.fast_fixed_shape_fallback_to_generic) {
            return fast_result;
        }
    }

    return GenericOrderDecisionImpl{}.decide(
        intent,
        bundle,
        depth_views,
        policy,
        now_ns,
        config_,
        &cache_
    );
}

}  // namespace trading_engine::order_decision
