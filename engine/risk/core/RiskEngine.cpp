#include "engine/risk/core/RiskEngine.h"

namespace trading_engine::risk {

RiskPipelineResult RiskEngine::evaluate(
    const signal::OpportunityIntent& intent,
    RiskEvaluationContext context
) {
    reservations_.expire_old(context.now_ns);
    context.ledger_snapshot = reservations_.snapshot();
    return pipeline_.evaluate(intent, context, &reservations_);
}

RiskLedgerSnapshot RiskEngine::ledger_snapshot() const {
    return reservations_.snapshot();
}

void RiskEngine::release_reservation(std::uint64_t reservation_id) {
    reservations_.release(reservation_id);
}

void RiskEngine::expire_old(std::uint64_t now_ns) {
    reservations_.expire_old(now_ns);
}

}  // namespace trading_engine::risk
