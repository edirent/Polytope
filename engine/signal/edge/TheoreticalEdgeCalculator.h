#pragma once

#include "engine/signal/edge/EdgeBreakdown.h"
#include "engine/signal/edge/LatencyBufferModel.h"
#include "engine/signal/pricing/CostResult.h"
#include "engine/signal/pricing/FeeModel.h"

namespace trading_engine::signal {

class TheoreticalEdgeCalculator {
public:
    TheoreticalEdgeCalculator(
        FeeModel fee_model,
        LatencyBufferModel latency_buffer_model
    );

    [[nodiscard]] EdgeBreakdown calculate(
        const CandidateBundle& bundle,
        const CostResult& cost
    ) const noexcept;

private:
    FeeModel fee_model_;
    LatencyBufferModel latency_buffer_model_;
};

}  // namespace trading_engine::signal
