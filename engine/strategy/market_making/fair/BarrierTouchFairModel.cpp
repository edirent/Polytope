#include "engine/strategy/market_making/fair/BarrierTouchFairModel.h"

#include <algorithm>
#include <cmath>

namespace trading_engine::strategy::market_making {

double BarrierTouchFairModel::normal_cdf(double x) noexcept {
    return 0.5 * std::erfc(-x / std::sqrt(2.0));
}

BarrierTouchFairResult BarrierTouchFairModel::compute(
    const BarrierTouchFairInput& input
) const noexcept {
    BarrierTouchFairResult result;

    if (!std::isfinite(input.spot) ||
        !std::isfinite(input.barrier) ||
        !std::isfinite(input.annualized_vol) ||
        !std::isfinite(input.tte_years) ||
        input.spot <= 0.0 ||
        input.barrier <= 0.0 ||
        input.annualized_vol <= 0.0 ||
        input.tte_years <= 0.0 ||
        input.price_scale_tick <= 0) {
        return result;
    }

    const bool is_up_touch =
        input.event_type == ExternalFairEventType::UpTouch;
    const bool is_down_touch =
        input.event_type == ExternalFairEventType::DownTouch;
    if (!is_up_touch && !is_down_touch) {
        return result;
    }

    const bool already_touched =
        (is_up_touch && input.spot >= input.barrier) ||
        (is_down_touch && input.spot <= input.barrier);
    if (already_touched) {
        result.ok = true;
        result.yes_probability = 1.0;
        result.fair_value_tick = input.price_scale_tick;
        return result;
    }

    const double log_distance = is_up_touch
        ? std::log(input.barrier / input.spot)
        : std::log(input.spot / input.barrier);
    if (!std::isfinite(log_distance) || log_distance <= 0.0) {
        result.ok = true;
        result.yes_probability = 1.0;
        result.fair_value_tick = input.price_scale_tick;
        return result;
    }

    const double expected_log_move =
        input.annualized_vol * std::sqrt(input.tte_years);
    if (!std::isfinite(expected_log_move) || expected_log_move <= 0.0) {
        return result;
    }

    const double z = log_distance / expected_log_move;
    if (!std::isfinite(z)) {
        return result;
    }

    // Driftless GBM / Brownian reflection approximation:
    // P(touch before T) ~= 2 * N(-z).
    const double p = std::clamp(2.0 * normal_cdf(-z), 0.0, 1.0);

    result.ok = true;
    result.yes_probability = p;
    result.fair_value_tick = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(
            std::llround(p * static_cast<double>(input.price_scale_tick))
        ),
        0,
        input.price_scale_tick
    );
    return result;
}

}  // namespace trading_engine::strategy::market_making
