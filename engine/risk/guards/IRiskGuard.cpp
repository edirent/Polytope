#include "engine/risk/guards/IRiskGuard.h"

namespace trading_engine::risk {

GuardResult pass_guard() {
    GuardResult result;
    result.pass = true;
    result.rejection = RiskDecisionType::Pass;
    result.reject_flag = 0;
    return result;
}

void record_guard_result(const GuardResult& result, RiskResult* risk_result) {
    if (risk_result == nullptr || result.pass) {
        return;
    }

    ++risk_result->intents_rejected;

    switch (result.rejection) {
        case RiskDecisionType::RejectKillSwitch:
            ++risk_result->rejected_kill_switch;
            break;
        case RiskDecisionType::RejectExpiredIntent:
            ++risk_result->rejected_stale_or_expired;
            break;
        case RiskDecisionType::RejectDuplicateIntent:
        case RiskDecisionType::RejectRateLimited:
            ++risk_result->rejected_pending_or_rate_limit;
            break;
        case RiskDecisionType::RejectBadMarketState:
            ++risk_result->rejected_bad_market_state;
            break;
        case RiskDecisionType::RejectStaleSnapshot:
            ++risk_result->rejected_snapshot_freshness;
            break;
        case RiskDecisionType::RejectInsufficientDepth:
            ++risk_result->rejected_cost_limit;
            break;
        case RiskDecisionType::RejectCostDrift:
            ++risk_result->rejected_drift_or_slippage;
            break;
        case RiskDecisionType::RejectReducedBundleQty:
            ++risk_result->rejected_inventory_limit;
            break;
        case RiskDecisionType::RejectLowTotalEdge:
        case RiskDecisionType::RejectLowUnitEdge:
        case RiskDecisionType::RejectLowEdgeBps:
            ++risk_result->rejected_low_edge;
            break;
        case RiskDecisionType::RejectCostLimit:
            ++risk_result->rejected_cost_limit;
            break;
        case RiskDecisionType::RejectTotalExposureLimit:
        case RiskDecisionType::RejectSingleMarketExposureLimit:
            ++risk_result->rejected_exposure_limit;
            break;
        case RiskDecisionType::RejectInventoryLimit:
            ++risk_result->rejected_inventory_limit;
            break;
        case RiskDecisionType::RejectPartialFillRisk:
            ++risk_result->rejected_partial_fill_risk;
            break;
        case RiskDecisionType::Pass:
        case RiskDecisionType::RejectInternalError:
            break;
    }
}

GuardResult run_risk_guards(
    std::span<IRiskGuard* const> guards,
    const signal::OpportunityIntent& intent,
    std::uint64_t now_ns
) {
    for (IRiskGuard* guard : guards) {
        if (guard == nullptr) {
            GuardResult result;
            result.pass = false;
            result.rejection = RiskDecisionType::RejectInternalError;
            result.reject_flag = kRiskRejectFlagInternalError;
            result.reason = "null risk guard";
            return result;
        }

        auto result = guard->check(intent, now_ns);
        if (!result.pass) {
            return result;
        }
    }

    return pass_guard();
}

}  // namespace trading_engine::risk
