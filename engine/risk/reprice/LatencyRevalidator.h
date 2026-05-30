#pragma once

#include "engine/signal/public/OpportunityIntent.h"

#include <cstdint>

namespace trading_engine::risk {

class LatencyRevalidator {
public:
    [[nodiscard]] std::int64_t estimate_latency_buffer_tick(
        const signal::OpportunityIntent& intent
    ) const noexcept;
};

}  // namespace trading_engine::risk
