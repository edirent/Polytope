#pragma once

#include "engine/strategy/market_making/canonical/CanonicalPriceMapper.h"
#include "engine/strategy/market_making/fair/ExternalFairMarketSpec.h"
#include "engine/strategy/market_making/fair/ExternalFairRuntime.h"
#include "engine/strategy/market_making/fair/SpotOracle.h"
#include "engine/strategy/market_making/fair/VolProvider.h"

#include <cstdint>
#include <string>

namespace trading_engine::strategy::market_making {

struct ExternalFairOutput {
    bool ok = false;
    std::int64_t canonical_yes_raw_fair_tick = 0;
    std::int64_t asset_raw_fair_tick = 0;
    double yes_probability = 0.0;
    double vol_used = 0.0;
    double spot_used = 0.0;
    std::int64_t tte_ns = 0;
    std::int64_t spot_age_ms = 0;
    std::int64_t vol_age_ms = 0;
    int confidence_bps = 0;
    std::string reject_reason;
};

class ExternalFairModel {
public:
    [[nodiscard]] ExternalFairOutput compute(
        const ExternalFairRuntime& runtime,
        const ExternalFairMarketSpec& spec,
        std::int64_t now_unix_ms,
        const SpotSnapshot& spot,
        const VolSnapshot& vol
    ) const;
};

}  // namespace trading_engine::strategy::market_making
