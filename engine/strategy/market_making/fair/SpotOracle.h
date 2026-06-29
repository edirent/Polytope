#pragma once

#include "engine/strategy/market_making/fair/ExternalFairMarketSpec.h"

#include <cstdint>

namespace trading_engine::strategy::market_making {

struct SpotSnapshot {
    bool ok = false;
    double spot = 0.0;

    std::int64_t exchange_ts_ms = 0;
    std::int64_t local_recv_ts_ms = 0;
};

class SpotOracle {
public:
    virtual ~SpotOracle() = default;

    [[nodiscard]] virtual SpotSnapshot latest(
        ExternalFairSymbol symbol
    ) const = 0;
};

}  // namespace trading_engine::strategy::market_making
