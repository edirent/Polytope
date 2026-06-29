#pragma once

#include "engine/strategy/market_making/fair/ExternalFairMarketSpec.h"

#include <cstdint>

namespace trading_engine::strategy::market_making {

struct VolSnapshot {
    bool ok = false;
    double annualized_vol = 0.0;
    std::int64_t update_ts_ms = 0;
};

class VolProvider {
public:
    virtual ~VolProvider() = default;

    [[nodiscard]] virtual VolSnapshot latest(
        ExternalFairSymbol symbol
    ) const = 0;
};

}  // namespace trading_engine::strategy::market_making
