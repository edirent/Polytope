#include "engine/strategy/market_making/products/up_down_5m/UpDown5mFairProvider.h"

#include "engine/strategy/market_making/fair/DigitalOptionFairModel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace trading_engine::strategy::market_making::products::up_down_5m {

namespace {

constexpr double kSecondsPerYear = 365.0 * 24.0 * 60.0 * 60.0;
constexpr double kMinFairProbability = 0.005;
constexpr double kMaxFairProbability = 0.995;

[[nodiscard]] std::int64_t probability_to_tick(double probability) noexcept {
    const auto clamped = std::clamp(
        probability,
        kMinFairProbability,
        kMaxFairProbability
    );
    return std::clamp<std::int64_t>(
        static_cast<std::int64_t>(
            std::llround(clamped * static_cast<double>(kPriceOneTick))
        ),
        1,
        kPriceOneTick - 1
    );
}

[[nodiscard]] std::int64_t z_score_bps(
    const UpDown5mMarketContext& context,
    double tau_years
) noexcept {
    if (context.current_spot <= 0.0 || context.start_spot <= 0.0 ||
        context.sigma_annualized <= 0.0) {
        return 0;
    }
    const auto log_moneyness =
        std::log(context.current_spot / context.start_spot);
    if (!std::isfinite(log_moneyness)) {
        return 0;
    }
    if (tau_years <= 0.0) {
        return log_moneyness == 0.0
            ? 0
            : std::numeric_limits<std::int64_t>::max();
    }
    const auto denominator = context.sigma_annualized * std::sqrt(tau_years);
    if (denominator <= 0.0 || !std::isfinite(denominator)) {
        return 0;
    }
    const auto z = std::fabs(log_moneyness) / denominator;
    if (!std::isfinite(z)) {
        return 0;
    }
    return std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::llround(z * 10'000.0)),
        0,
        std::numeric_limits<std::int64_t>::max()
    );
}

}  // namespace

UpDown5mFairOutput UpDown5mFairProvider::compute(
    const UpDown5mMarketContext& context
) const noexcept {
    UpDown5mFairOutput output;
    if (context.asset_direction != ProductDirection::Up &&
        context.asset_direction != ProductDirection::Down) {
        output.reject_reason = "unknown_asset_direction";
        return output;
    }
    if (context.start_spot <= 0.0 || context.current_spot <= 0.0 ||
        context.sigma_annualized <= 0.0 ||
        !std::isfinite(context.start_spot) ||
        !std::isfinite(context.current_spot) ||
        !std::isfinite(context.sigma_annualized)) {
        output.reject_reason = "bad_terminal_digital_inputs";
        return output;
    }

    const auto tau_ms = std::max<std::int64_t>(
        0,
        context.round_expiry_ms - context.now_ms
    );
    const auto tau_ns = static_cast<std::uint64_t>(tau_ms) * 1'000'000ULL;
    const auto tau_years =
        (static_cast<double>(tau_ms) / 1000.0) / kSecondsPerYear;

    const auto digital = DigitalOptionFairModel{}.compute(
        DigitalOptionFairInput{
            .spot = context.current_spot,
            .strike = context.start_spot,
            .vol_annual_bps = context.sigma_annualized * 10'000.0,
            .drift_annual_bps = 0.0,
            .time_to_expiry_ns = tau_ns,
            .invert = false
        }
    );
    if (!digital.ok || !std::isfinite(digital.probability)) {
        output.reject_reason = "terminal_digital_model_failed";
        return output;
    }

    output.ok = true;
    output.tau_ms = static_cast<std::uint64_t>(tau_ms);
    output.up_probability = std::clamp(
        digital.probability,
        kMinFairProbability,
        kMaxFairProbability
    );
    output.up_fair_tick = probability_to_tick(output.up_probability);
    output.down_fair_tick = kPriceOneTick - output.up_fair_tick;
    output.asset_fair_tick =
        context.asset_direction == ProductDirection::Up
            ? output.up_fair_tick
            : output.down_fair_tick;
    output.z_score_bps = z_score_bps(context, tau_years);
    return output;
}

}  // namespace trading_engine::strategy::market_making::products::up_down_5m
