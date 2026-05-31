#include "engine/risk/validate/IntentValidator.h"

#include <utility>

namespace trading_engine::risk {

namespace {

[[nodiscard]] IntentValidationResult reject(
    RiskRejectReason reason,
    std::string detail
) {
    IntentValidationResult result;
    result.ok = false;
    result.reject_reason = reason;
    result.detail = std::move(detail);
    return result;
}

[[nodiscard]] IntentValidationResult accept() {
    IntentValidationResult result;
    result.ok = true;
    result.reject_reason = RiskRejectReason::None;
    return result;
}

}  // namespace

IntentValidationResult IntentValidator::validate(
    const signal::OpportunityIntent& intent,
    std::uint64_t now_ns
) const {
    if (intent.intent_id == 0) {
        return reject(RiskRejectReason::InvalidIntent, "missing intent_id");
    }
    if (intent.bundle_id == 0) {
        return reject(RiskRejectReason::InvalidIntent, "missing bundle_id");
    }
    if (intent.status != signal::IntentStatus::PaperOpportunity) {
        return reject(
            RiskRejectReason::InvalidIntent,
            "intent is not PaperOpportunity"
        );
    }
    if (intent.bundle_qty <= 0) {
        return reject(RiskRejectReason::InvalidIntent, "missing bundle_qty");
    }
    if (intent.total_edge_tick <= 0) {
        return reject(
            RiskRejectReason::LowTotalEdge,
            "missing or non-positive total_edge_tick"
        );
    }
    if (intent.expires_at_ns <= now_ns) {
        return reject(RiskRejectReason::ExpiredIntent, "intent expired");
    }
    if (intent.idempotency_hash == 0 && intent.idempotency_key.empty()) {
        return reject(
            RiskRejectReason::MissingEvidence,
            "missing idempotency evidence"
        );
    }
    if (intent.leg_count == 0 || intent.leg_count > signal::kMaxIntentLegs) {
        return reject(RiskRejectReason::InvalidIntent, "invalid leg_count");
    }

    return accept();
}

}  // namespace trading_engine::risk
