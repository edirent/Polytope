#pragma once

#include "engine/risk/ledger/RiskLedger.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/risk/reprice/CostRevalidationResult.h"
#include "engine/signal/public/OpportunityIntent.h"

#include <cstdint>
#include <string>

namespace trading_engine::risk {

struct InventoryGuardResult {
    bool pass = false;
    RiskDecisionType rejection = RiskDecisionType::RejectInternalError;

    std::string rejected_asset_id;
    std::int64_t post_asset_lots = 0;

    std::string reason;
};

class InventoryGuard {
public:
    [[nodiscard]] InventoryGuardResult check(
        const RiskLedgerSnapshot& ledger,
        const signal::OpportunityIntent& intent,
        const CostRevalidationResult& cost,
        const RiskPolicySnapshot& policy
    ) const;
};

}  // namespace trading_engine::risk
