#include "engine/risk/reprice/SlippageRevalidator.h"

namespace trading_engine::risk {

std::int64_t SlippageRevalidator::estimate_slippage_buffer_tick(
    const signal::OpportunityIntent& intent
) const noexcept {
    return intent.slippage_buffer_tick;
}

}  // namespace trading_engine::risk
