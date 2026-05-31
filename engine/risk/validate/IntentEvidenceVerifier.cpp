#include "engine/risk/validate/IntentEvidenceVerifier.h"

#include <utility>

namespace trading_engine::risk {

namespace {

[[nodiscard]] IntentValidationResult reject(std::string detail) {
    IntentValidationResult result;
    result.ok = false;
    result.reject_reason = RiskRejectReason::MissingEvidence;
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

IntentValidationResult IntentEvidenceVerifier::verify(
    const signal::OpportunityIntent& intent
) const {
    if (intent.oracle_artifact_hash == 0) {
        return reject("missing oracle_artifact_hash");
    }
    if (intent.bundle_hash == 0) {
        return reject("missing bundle_hash");
    }
    if (intent.snapshot_version_hash == 0) {
        return reject("missing snapshot_version_hash");
    }
    if (intent.created_ts_ns == 0) {
        return reject("missing created_ts_ns");
    }
    if (intent.expires_at_ns == 0) {
        return reject("missing expires_at_ns");
    }
    if (intent.expires_at_ns <= intent.created_ts_ns) {
        return reject("expires_at_ns must be greater than created_ts_ns");
    }
    if (intent.idempotency_hash == 0 && intent.idempotency_key.empty()) {
        return reject("missing idempotency evidence");
    }

    return accept();
}

}  // namespace trading_engine::risk
