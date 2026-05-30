#include "engine/risk/reprice/FeeRevalidator.h"

namespace trading_engine::risk {

std::int64_t FeeRevalidator::estimate_fee_tick(
    const signal::OpportunityIntent& intent
) const noexcept {
    return intent.estimated_fee_tick;
}

}  // namespace trading_engine::risk
