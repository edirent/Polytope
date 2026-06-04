#pragma once

#include "engine/strategy/market_making/public/MarketMakingConfig.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace trading_engine::strategy::market_making {

class InventorySkewModel {
public:
    [[nodiscard]] std::int64_t compute(
        const MarketMakingConfig& config,
        std::int64_t current_position_lots,
        std::uint64_t time_to_expiry_ns = 0
    ) const noexcept {
        if (config.max_inventory_lots <= 0 ||
            config.max_inventory_skew_tick == 0) {
            return 0;
        }
        const auto delta =
            current_position_lots - config.target_position_lots;
        const auto denominator = delta >= 0
            ? std::max<std::int64_t>(
                  1,
                  config.max_inventory_lots - config.target_position_lots
              )
            : std::max<std::int64_t>(
                  1,
                  config.target_position_lots - config.min_inventory_lots
              );
        const auto ratio = std::clamp(
            static_cast<long double>(delta) /
                static_cast<long double>(denominator),
            -1.0L,
            1.0L
        );
        const auto pressure = nonlinear_pressure(config, std::fabs(ratio));
        const auto raw_skew =
            (ratio < 0.0L ? -pressure : pressure) *
            static_cast<long double>(config.max_inventory_skew_tick);
        return clamp_round(
            raw_skew * tte_multiplier(config, time_to_expiry_ns)
        );
    }

private:
    [[nodiscard]] static long double nonlinear_pressure(
        const MarketMakingConfig& config,
        long double pressure
    ) noexcept {
        pressure = std::clamp(pressure, 0.0L, 1.0L);
        const auto exponent =
            std::max(1.0L, static_cast<long double>(
                               config.inventory_skew_exponent
                           ));
        if (exponent <= 1.0L) {
            return pressure;
        }

        const auto start = std::clamp(
            static_cast<long double>(
                config.inventory_skew_nonlinear_start_bps
            ) / 10'000.0L,
            0.0L,
            1.0L
        );
        if (pressure <= start) {
            return pressure;
        }
        if (start >= 1.0L) {
            return pressure;
        }

        const auto normalized = (pressure - start) / (1.0L - start);
        const auto curved =
            1.0L - std::pow(1.0L - normalized, exponent);
        return std::clamp(
            start + curved * (1.0L - start),
            0.0L,
            1.0L
        );
    }

    [[nodiscard]] static long double tte_multiplier(
        const MarketMakingConfig& config,
        std::uint64_t time_to_expiry_ns
    ) noexcept {
        if (time_to_expiry_ns == 0 || config.tte_skew_start_ns == 0 ||
            config.tte_max_skew_multiplier <= 1.0) {
            return 1.0L;
        }
        if (time_to_expiry_ns > config.tte_skew_start_ns) {
            return 1.0L;
        }

        const auto max_multiplier = std::max(
            1.0L,
            static_cast<long double>(config.tte_max_skew_multiplier)
        );
        const auto puke_start =
            std::min(config.tte_puke_start_ns, config.tte_skew_start_ns);
        if (puke_start > 0 && time_to_expiry_ns <= puke_start) {
            return max_multiplier;
        }

        const auto denominator_ns = config.tte_skew_start_ns - puke_start;
        const auto progress = denominator_ns == 0
            ? 1.0L
            : std::clamp(
                  static_cast<long double>(
                      config.tte_skew_start_ns - time_to_expiry_ns
                  ) / static_cast<long double>(denominator_ns),
                  0.0L,
                  1.0L
              );
        constexpr long double kCurveSteepness = 4.0L;
        const auto curve =
            (std::exp(kCurveSteepness * progress) - 1.0L) /
            (std::exp(kCurveSteepness) - 1.0L);
        return 1.0L + curve * (max_multiplier - 1.0L);
    }

    [[nodiscard]] static std::int64_t clamp_round(
        long double value
    ) noexcept {
        const auto max_value = static_cast<long double>(
            std::numeric_limits<std::int64_t>::max()
        );
        const auto min_value = static_cast<long double>(
            std::numeric_limits<std::int64_t>::min()
        );
        if (value >= max_value) {
            return std::numeric_limits<std::int64_t>::max();
        }
        if (value <= min_value) {
            return std::numeric_limits<std::int64_t>::min();
        }
        return static_cast<std::int64_t>(std::llround(value));
    }
};

}  // namespace trading_engine::strategy::market_making
