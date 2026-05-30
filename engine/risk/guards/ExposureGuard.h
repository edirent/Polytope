#pragma once

#include "engine/risk/ledger/RiskLedger.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/risk/reprice/CostRevalidationResult.h"
#include "engine/signal/public/OpportunityIntent.h"

#include <cstdint>
#include <string>

namespace trading_engine::risk {

struct ExposureGuardResult {
    bool pass = false;
    RiskDecisionType rejection = RiskDecisionType::RejectInternalError;

    std::int64_t post_total_exposure_tick = 0;
    std::string rejected_market_id;
    std::int64_t post_market_exposure_tick = 0;

    std::string reason;
};

class ExposureGuard {
public:
    [[nodiscard]] ExposureGuardResult check(
        const RiskLedgerSnapshot& ledger,
        const signal::OpportunityIntent& intent,
        const CostRevalidationResult& cost,
        const RiskPolicySnapshot& policy
    ) const;
};

}  // namespace trading_engine::risk
