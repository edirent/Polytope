#include "engine/risk/guards/IntentExpiryGuard.h"

namespace trading_engine::risk {

GuardResult IntentExpiryGuard::check(
    const signal::OpportunityIntent& intent,
    std::uint64_t now_ns
) {
    if (intent.expires_at_ns > now_ns) {
        return pass_guard();
    }

    GuardResult result;
    result.pass = false;
    result.rejection = RiskDecisionType::RejectExpiredIntent;
    result.reject_flag = kRiskRejectFlagExpiredIntent;
    result.reason = "intent expired";
    return result;
}

}  // namespace trading_engine::risk
