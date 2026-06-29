#pragma once

#include "engine/strategy/market_making/canonical/CanonicalPriceMapper.h"
#include "engine/strategy/market_making/fair/ExternalFairModel.h"
#include "engine/strategy/market_making/fair/MarketImpliedFairModel.h"
#include "engine/strategy/market_making/public/MarketMakingTypes.h"

#include <cstdint>
#include <string>

namespace trading_engine::strategy::market_making {

struct TradableFairInput {
    OutcomeSide asset_side = OutcomeSide::Yes;
    std::int64_t price_scale_tick = kPriceOneTick;
    double lambda = 0.20;
    bool shadow_only = false;
    ExternalFairOutput external;
    MarketImpliedFairOutput market;
};

struct TradableFairOutput {
    bool ok = false;
    std::int64_t canonical_yes_market_fair_tick = 0;
    std::int64_t canonical_yes_raw_external_fair_tick = 0;
    std::int64_t canonical_yes_tradable_fair_tick = 0;
    std::int64_t asset_raw_external_fair_tick = 0;
    std::int64_t asset_tradable_fair_tick = 0;
    std::int64_t canonical_yes_basis_tick = 0;
    int confidence_bps = 0;
    double lambda_used = 0.0;
    std::string reason;
};

class TradableFairBuilder {
public:
    [[nodiscard]] TradableFairOutput build(
        const TradableFairInput& input
    ) const noexcept;
};

}  // namespace trading_engine::strategy::market_making
