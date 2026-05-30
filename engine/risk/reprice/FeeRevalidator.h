#pragma once

#include "engine/signal/public/OpportunityIntent.h"

#include <cstdint>

namespace trading_engine::risk {

class FeeRevalidator {
public:
    [[nodiscard]] std::int64_t estimate_fee_tick(
        const signal::OpportunityIntent& intent
    ) const noexcept;
};

}  // namespace trading_engine::risk
