#include "engine/strategy/market_making/fair/TradableFairBuilder.h"

#include <algorithm>
#include <cmath>

namespace trading_engine::strategy::market_making {

TradableFairOutput TradableFairBuilder::build(
    const TradableFairInput& input
) const noexcept {
    TradableFairOutput output;
    if (!input.market.ok) {
        output.reason = "market_implied_fair_unavailable";
        return output;
    }

    output.ok = true;
    output.canonical_yes_market_fair_tick =
        input.market.canonical_yes_market_mid_tick;
    output.canonical_yes_tradable_fair_tick =
        input.market.canonical_yes_market_mid_tick;
    output.confidence_bps = input.market.confidence_bps;
    output.reason = "market_implied_only";

    if (input.external.ok && input.external.confidence_bps > 0) {
        const auto bounded_lambda = std::clamp(input.lambda, 0.0, 1.0);
        const auto confidence =
            std::clamp(input.external.confidence_bps, 0, 10'000) / 10'000.0;
        const auto effective_lambda =
            input.shadow_only ? 0.0 : bounded_lambda * confidence;
        const auto market = input.market.canonical_yes_market_mid_tick;
        const auto external = input.external.canonical_yes_raw_fair_tick;
        const auto blended = static_cast<std::int64_t>(
            std::llround(
                static_cast<double>(market) +
                effective_lambda * static_cast<double>(external - market)
            )
        );
        output.canonical_yes_raw_external_fair_tick = external;
        output.asset_raw_external_fair_tick = input.external.asset_raw_fair_tick;
        output.canonical_yes_tradable_fair_tick =
            clamp_price_tick(blended, input.price_scale_tick);
        output.canonical_yes_basis_tick = external - market;
        output.confidence_bps =
            std::min(input.market.confidence_bps, input.external.confidence_bps);
        output.lambda_used = effective_lambda;
        output.reason = input.shadow_only ? "external_shadow_only"
                                          : "external_blend";
    }

    output.asset_tradable_fair_tick = canonical_yes_to_asset_tick(
        input.asset_side,
        output.canonical_yes_tradable_fair_tick,
        input.price_scale_tick
    );
    if (output.asset_raw_external_fair_tick == 0 &&
        output.canonical_yes_raw_external_fair_tick > 0) {
        output.asset_raw_external_fair_tick = canonical_yes_to_asset_tick(
            input.asset_side,
            output.canonical_yes_raw_external_fair_tick,
            input.price_scale_tick
        );
    }
    return output;
}

}  // namespace trading_engine::strategy::market_making
