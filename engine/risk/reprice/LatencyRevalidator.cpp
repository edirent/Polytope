#include "engine/risk/reprice/LatencyRevalidator.h"

namespace trading_engine::risk {

std::int64_t LatencyRevalidator::estimate_latency_buffer_tick(
    const signal::OpportunityIntent& intent
) const noexcept {
    return intent.latency_buffer_tick;
}

}  // namespace trading_engine::risk
