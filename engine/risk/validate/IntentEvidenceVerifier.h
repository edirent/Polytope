#pragma once

#include "engine/risk/validate/IntentValidator.h"
#include "engine/signal/public/OpportunityIntent.h"

namespace trading_engine::risk {

class IntentEvidenceVerifier {
public:
    [[nodiscard]] IntentValidationResult verify(
        const signal::OpportunityIntent& intent
    ) const;
};

}  // namespace trading_engine::risk
