#pragma once

#include "engine/strategy/market_making/fair/ExternalFairMarketSpec.h"

#include <cstdint>

namespace trading_engine::strategy::market_making {

struct BarrierTouchFairInput {
    double spot = 0.0;
    double barrier = 0.0;
    double annualized_vol = 0.0;
    double tte_years = 0.0;

    ExternalFairEventType event_type = ExternalFairEventType::Unknown;
    std::int64_t price_scale_tick = 10'000;
};

struct BarrierTouchFairResult {
    bool ok = false;
    double yes_probability = 0.0;
    std::int64_t fair_value_tick = 0;
};

class BarrierTouchFairModel {
public:
    [[nodiscard]] BarrierTouchFairResult compute(
        const BarrierTouchFairInput& input
    ) const noexcept;

private:
    [[nodiscard]] static double normal_cdf(double x) noexcept;
};

}  // namespace trading_engine::strategy::market_making
