#pragma once

#include "engine/risk/guards/IRiskGuard.h"

namespace trading_engine::risk {

class IntentExpiryGuard final : public IRiskGuard {
public:
    GuardResult check(
        const signal::OpportunityIntent& intent,
        std::uint64_t now_ns
    ) override;
};

}  // namespace trading_engine::risk
