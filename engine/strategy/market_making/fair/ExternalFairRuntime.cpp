#include "engine/strategy/market_making/fair/ExternalFairRuntime.h"

#include <algorithm>
#include <cmath>

namespace trading_engine::strategy::market_making {

namespace {

constexpr double kSecondsPerYear = 365.25 * 24.0 * 60.0 * 60.0;

[[nodiscard]] bool is_touch_event(ExternalFairEventType type) noexcept {
    return type == ExternalFairEventType::UpTouch ||
           type == ExternalFairEventType::DownTouch;
}

}  // namespace

ExternalFairRuntime::ExternalFairRuntime(
    const SpotOracle& spot_oracle,
    const VolProvider& vol_provider
)
    : spot_oracle_(spot_oracle),
      vol_provider_(vol_provider) {}

ExternalFairResult ExternalFairRuntime::compute(
    const ExternalFairMarketSpec& spec,
    std::int64_t now_ms
) const {
    if (spec.symbol == ExternalFairSymbol::Unknown) {
        return ExternalFairResult::reject("unknown_symbol");
    }
    if (spec.event_type == ExternalFairEventType::Unknown) {
        return ExternalFairResult::reject("unknown_event_type");
    }
    if (spec.barrier_price <= 0.0 || !std::isfinite(spec.barrier_price)) {
        return ExternalFairResult::reject("bad_barrier");
    }
    if (spec.expiry_unix_ms <= now_ms) {
        return ExternalFairResult::reject("expired");
    }
    if (spec.price_scale_tick <= 0) {
        return ExternalFairResult::reject("bad_price_scale");
    }

    const auto spot = spot_oracle_.latest(spec.symbol);
    if (!spot.ok || spot.spot <= 0.0 || !std::isfinite(spot.spot)) {
        return ExternalFairResult::reject("missing_spot");
    }
    if (now_ms - spot.local_recv_ts_ms > spec.max_spot_staleness_ms) {
        return ExternalFairResult::reject("stale_spot");
    }

    const auto vol = vol_provider_.latest(spec.symbol);
    if (!vol.ok ||
        vol.annualized_vol <= 0.0 ||
        !std::isfinite(vol.annualized_vol)) {
        return ExternalFairResult::reject("missing_vol");
    }
    if (now_ms - vol.update_ts_ms > spec.max_vol_staleness_ms) {
        return ExternalFairResult::reject("stale_vol");
    }
    if (vol.annualized_vol < spec.min_annualized_vol ||
        vol.annualized_vol > spec.max_annualized_vol) {
        return ExternalFairResult::reject("vol_out_of_bounds");
    }

    const double tte_seconds =
        static_cast<double>(spec.expiry_unix_ms - now_ms) / 1000.0;
    const double tte_years = tte_seconds / kSecondsPerYear;
    if (tte_years <= 0.0 || !std::isfinite(tte_years)) {
        return ExternalFairResult::reject("bad_tte");
    }

    if (!is_touch_event(spec.event_type)) {
        return ExternalFairResult::reject("unsupported_non_touch_event");
    }

    const auto touch = barrier_touch_model_.compute(
        BarrierTouchFairInput{
            .spot = spot.spot,
            .barrier = spec.barrier_price,
            .annualized_vol = vol.annualized_vol,
            .tte_years = tte_years,
            .event_type = spec.event_type,
            .price_scale_tick = spec.price_scale_tick
        }
    );
    if (!touch.ok) {
        return ExternalFairResult::reject("barrier_touch_model_failed");
    }

    auto token_fair_tick = touch.fair_value_tick;
    if (spec.outcome_side == OutcomeSide::No) {
        token_fair_tick = spec.price_scale_tick - token_fair_tick;
    }
    token_fair_tick = std::clamp<std::int64_t>(
        token_fair_tick,
        0,
        spec.price_scale_tick
    );

    return ExternalFairResult::success(
        token_fair_tick,
        touch.yes_probability
    );
}

}  // namespace trading_engine::strategy::market_making
