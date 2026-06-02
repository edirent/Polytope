#pragma once

#include "engine/strategy/market_making/public/MarketMakingTypes.h"

namespace trading_engine::strategy::market_making {

struct FairValueSource {
    FairValueSourceKind kind = FairValueSourceKind::Mid;
    std::uint64_t source_hash = 0;
};

}  // namespace trading_engine::strategy::market_making
