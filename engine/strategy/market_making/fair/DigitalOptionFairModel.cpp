#include "engine/strategy/market_making/fair/DigitalOptionFairModel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace trading_engine::strategy::market_making {

namespace {

constexpr double kSecondsPerYear = 365.0 * 24.0 * 60.0 * 60.0;
constexpr double kInvSqrtTwoPi = 0.39894228040143267794;

[[nodiscard]] double normal_cdf(double value) noexcept {
    return 0.5 * std::erfc(-value / std::sqrt(2.0));
}

[[nodiscard]] double normal_pdf(double value) noexcept {
    return kInvSqrtTwoPi * std::exp(-0.5 * value * value);
}

[[nodiscard]] std::int64_t probability_to_tick(double probability) noexcept {
    const auto clamped = std::clamp(probability, 0.0, 1.0);
    return std::clamp<std::int64_t>(
        static_cast<std::int64_t>(
            std::llround(clamped * static_cast<double>(kPriceOneTick))
        ),
        1,
        kPriceOneTick - 1
    );
}

[[nodiscard]] std::int64_t probability_width_to_tick(double value) noexcept {
    if (!std::isfinite(value) || value <= 0.0) {
        return 0;
    }
    return std::clamp<std::int64_t>(
        static_cast<std::int64_t>(
            std::llround(value * static_cast<double>(kPriceOneTick))
        ),
        0,
        kPriceOneTick - 1
    );
}

[[nodiscard]] std::int64_t clamp_dynamic_tick(
    std::int64_t value,
    std::int64_t min_value,
    std::int64_t max_value
) noexcept {
    auto out = std::max<std::int64_t>(0, value);
    if (min_value > 0) {
        out = std::max(out, min_value);
    }
    if (max_value > 0) {
        out = std::min(out, max_value);
    }
    return out;
}

}  // namespace

DigitalOptionFairResult DigitalOptionFairModel::compute(
    const DigitalOptionFairInput& input
) const noexcept {
    DigitalOptionFairResult result;
    if (input.spot <= 0.0 || input.strike <= 0.0 ||
        input.vol_annual_bps <= 0.0) {
        return result;
    }

    const auto seconds =
        static_cast<double>(input.time_to_expiry_ns) / 1'000'000'000.0;
    result.time_to_expiry_years = seconds / kSecondsPerYear;

    if (result.time_to_expiry_years <= 0.0) {
        result.probability = input.spot > input.strike ? 1.0 : 0.0;
        if (input.invert) {
            result.probability = 1.0 - result.probability;
        }
        result.fair_value_tick = probability_to_tick(result.probability);
        result.ok = true;
        return result;
    }

    const auto sigma = input.vol_annual_bps / 10'000.0;
    const auto drift = input.drift_annual_bps / 10'000.0;
    const auto sqrt_t = std::sqrt(result.time_to_expiry_years);
    const auto denom = sigma * sqrt_t;
    if (denom <= 0.0 || !std::isfinite(denom)) {
        return result;
    }

    result.d2 =
        (std::log(input.spot / input.strike) +
         (drift - 0.5 * sigma * sigma) * result.time_to_expiry_years) /
        denom;
    if (!std::isfinite(result.d2)) {
        return result;
    }

    result.pdf_d2 = normal_pdf(result.d2);
    result.probability = normal_cdf(result.d2);
    if (input.invert) {
        result.probability = 1.0 - result.probability;
    }
    result.fair_value_tick = probability_to_tick(result.probability);

    // Local variance proxy for the binary probability over the remaining
    // horizon. This keeps AS sizing in probability space instead of BTC price
    // space and is intentionally capped to the binary payoff range.
    result.probability_variance_to_expiry =
        std::clamp(result.pdf_d2 * result.pdf_d2, 0.0, 0.25);
    result.ok = true;
    return result;
}

AvellanedaStoikovResult AvellanedaStoikovModel::compute(
    const AvellanedaStoikovInput& input
) const noexcept {
    AvellanedaStoikovResult result;
    if (!input.fair.ok || input.risk_aversion <= 0.0 ||
        input.order_arrival_k < 0.0) {
        return result;
    }

    const auto variance = std::clamp(
        input.fair.probability_variance_to_expiry,
        0.0,
        0.25
    );
    const auto risk_term = input.risk_aversion * variance;
    const auto arrival_term =
        std::log1p(input.risk_aversion * input.order_arrival_k / 2.0) /
        input.risk_aversion;
    auto half_spread_probability = risk_term + arrival_term;
    if (!std::isfinite(half_spread_probability) ||
        half_spread_probability < 0.0) {
        return result;
    }
    half_spread_probability *= std::max(1.0, input.spread_multiplier);

    result.reservation_risk_tick = probability_width_to_tick(risk_term);
    result.half_spread_tick = clamp_dynamic_tick(
        probability_width_to_tick(half_spread_probability),
        input.min_half_spread_tick,
        input.max_half_spread_tick
    );
    result.max_inventory_skew_tick = clamp_dynamic_tick(
        result.reservation_risk_tick,
        input.min_inventory_skew_tick,
        input.max_inventory_skew_tick
    );
    result.ok = result.half_spread_tick > 0;
    return result;
}

}  // namespace trading_engine::strategy::market_making
