#pragma once

#include "engine/risk/core/RiskContext.h"
#include "engine/risk/core/RiskPipeline.h"
#include "engine/risk/ledger/ReservationBook.h"

namespace trading_engine::risk {

class RiskEngine {
public:
    [[nodiscard]] RiskPipelineResult evaluate(
        const signal::OpportunityIntent& intent,
        RiskEvaluationContext context
    );

    [[nodiscard]] RiskLedgerSnapshot ledger_snapshot() const;

    void release_reservation(std::uint64_t reservation_id);
    void expire_old(std::uint64_t now_ns);

private:
    ReservationBook reservations_;
    RiskPipeline pipeline_;
};

}  // namespace trading_engine::risk
