#pragma once

#include "engine/signal/public/OpportunityIntent.h"

#include <cstdint>

namespace trading_engine::risk {

class SlippageRevalidator {
public:
    [[nodiscard]] std::int64_t estimate_slippage_buffer_tick(
        const signal::OpportunityIntent& intent
    ) const noexcept;
};

}  // namespace trading_engine::risk
