#include "engine/strategy/market_making/fair/MarketImpliedFairModel.h"

namespace trading_engine::strategy::market_making {

MarketImpliedFairOutput MarketImpliedFairModel::compute(
    const CanonicalMarketState& state
) const noexcept {
    MarketImpliedFairOutput output;
    if (state.canonical_yes_bid_tick <= 0 ||
        state.canonical_yes_ask_tick <= 0 ||
        state.canonical_yes_ask_tick < state.canonical_yes_bid_tick) {
        return output;
    }
    output.ok = true;
    output.canonical_yes_market_mid_tick = state.canonical_yes_mid_tick;
    output.implied_fair_tick = state.canonical_yes_mid_tick;
    output.confidence_bps = 10'000;
    return output;
}

}  // namespace trading_engine::strategy::market_making
