#include "engine/signal/pricing/FeeModel.h"

namespace trading_engine::signal {

FeeModel::FeeModel(std::int64_t default_fee_tick)
    : default_fee_tick_(default_fee_tick) {}

std::int64_t FeeModel::estimate_fee_tick(
    const CandidateBundle&,
    const CostResult&
) const noexcept {
    return default_fee_tick_;
}

}  // namespace trading_engine::signal
