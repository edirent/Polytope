#include "engine/strategy/market_making/fair/ExternalFairModel.h"

#include <cmath>

namespace trading_engine::strategy::market_making {

ExternalFairOutput ExternalFairModel::compute(
    const ExternalFairRuntime& runtime,
    const ExternalFairMarketSpec& spec,
    std::int64_t now_unix_ms,
    const SpotSnapshot& spot,
    const VolSnapshot& vol
) const {
    ExternalFairOutput output;
    const auto result = runtime.compute(spec, now_unix_ms);
    if (!result.ok) {
        output.reject_reason = result.reject_reason;
        return output;
    }

    output.ok = true;
    output.asset_raw_fair_tick = result.fair_value_tick;
    output.canonical_yes_raw_fair_tick = asset_to_canonical_yes_tick(
        spec.outcome_side,
        result.fair_value_tick,
        spec.price_scale_tick
    );
    output.yes_probability = result.yes_probability;
    output.vol_used = vol.annualized_vol;
    output.spot_used = spot.spot;
    output.tte_ns = spec.expiry_unix_ms > now_unix_ms
        ? (spec.expiry_unix_ms - now_unix_ms) * 1'000'000LL
        : 0;
    output.spot_age_ms = spot.local_recv_ts_ms > 0
        ? now_unix_ms - spot.local_recv_ts_ms
        : 0;
    output.vol_age_ms = vol.update_ts_ms > 0
        ? now_unix_ms - vol.update_ts_ms
        : 0;
    output.confidence_bps = 10'000;
    if (output.spot_age_ms > spec.max_spot_staleness_ms ||
        output.vol_age_ms > spec.max_vol_staleness_ms ||
        !std::isfinite(output.yes_probability)) {
        output.confidence_bps = 0;
    }
    return output;
}

}  // namespace trading_engine::strategy::market_making
