#pragma once

#include "engine/signal/pricing/CostResult.h"
#include "engine/signal/reader/OracleArtifactReader.h"

#include <cstdint>

namespace trading_engine::signal {

class FeeModel {
public:
    explicit FeeModel(std::int64_t default_fee_tick);

    [[nodiscard]] std::int64_t estimate_fee_tick(
        const CandidateBundle& bundle,
        const CostResult& cost
    ) const noexcept;

private:
    std::int64_t default_fee_tick_ = 0;
};

}  // namespace trading_engine::signal
