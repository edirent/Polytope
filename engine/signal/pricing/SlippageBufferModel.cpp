#include "engine/signal/pricing/SlippageBufferModel.h"

namespace trading_engine::signal {

SlippageBufferModel::SlippageBufferModel(
    std::int64_t default_slippage_buffer_tick
) : default_slippage_buffer_tick_(default_slippage_buffer_tick) {}

std::int64_t SlippageBufferModel::estimate_slippage_buffer_tick(
    const CandidateBundle&,
    const CostResult&,
    const SignalConfig& config
) const noexcept {
    if (config.slippage_buffer_tick != 0) {
        return config.slippage_buffer_tick;
    }
    return default_slippage_buffer_tick_;
}

}  // namespace trading_engine::signal
