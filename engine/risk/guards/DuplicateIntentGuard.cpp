#include "engine/risk/guards/DuplicateIntentGuard.h"

namespace trading_engine::risk {

GuardResult DuplicateIntentGuard::check(
    const signal::OpportunityIntent& intent,
    std::uint64_t
) {
    if (intent.idempotency_key.empty()) {
        GuardResult result;
        result.pass = false;
        result.rejection = RiskDecisionType::RejectInternalError;
        result.reject_flag = kRiskRejectFlagInternalError;
        result.reason = "missing idempotency_key";
        return result;
    }

    const auto [_, inserted] =
        seen_idempotency_keys_.insert(intent.idempotency_key);
    if (inserted) {
        return pass_guard();
    }

    GuardResult result;
    result.pass = false;
    result.rejection = RiskDecisionType::RejectDuplicateIntent;
    result.reject_flag = kRiskRejectFlagDuplicateIntent;
    result.reason = "duplicate idempotency_key";
    return result;
}

void DuplicateIntentGuard::clear() {
    seen_idempotency_keys_.clear();
}

}  // namespace trading_engine::risk
