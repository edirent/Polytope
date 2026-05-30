#pragma once

#include "engine/signal/pricing/CostResult.h"
#include "engine/signal/public/SignalConfig.h"
#include "engine/signal/reader/OracleArtifactReader.h"

#include <cstdint>

namespace trading_engine::signal {

class SlippageBufferModel {
public:
    explicit SlippageBufferModel(std::int64_t default_slippage_buffer_tick);

    [[nodiscard]] std::int64_t estimate_slippage_buffer_tick(
        const CandidateBundle& bundle,
        const CostResult& cost,
        const SignalConfig& config
    ) const noexcept;

private:
    std::int64_t default_slippage_buffer_tick_ = 0;
};

}  // namespace trading_engine::signal
