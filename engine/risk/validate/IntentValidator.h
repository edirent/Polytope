#pragma once

#include "engine/risk/public/RiskTypes.h"
#include "engine/signal/public/OpportunityIntent.h"

#include <cstdint>
#include <string>

namespace trading_engine::risk {

struct IntentValidationResult {
    bool ok = false;
    RiskRejectReason reject_reason = RiskRejectReason::NotEvaluated;
    std::string detail;
};

class IntentValidator {
public:
    [[nodiscard]] IntentValidationResult validate(
        const signal::OpportunityIntent& intent,
        std::uint64_t now_ns
    ) const;
};

}  // namespace trading_engine::risk
