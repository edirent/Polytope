#pragma once

#include "engine/paper/metrics/SharpeCalculator.h"

#include <algorithm>
#include <cstdint>
#include <span>

namespace trading_engine::paper {

struct DrawdownResult {
    std::int64_t max_drawdown_tick = 0;
    double max_drawdown_ratio = 0.0;
    MetricStatus status = MetricStatus::InsufficientData;
};

class DrawdownCalculator {
public:
    [[nodiscard]] DrawdownResult compute(
        std::span<const std::int64_t> equity_ticks
    ) const {
        if (equity_ticks.empty()) {
            return {};
        }

        auto peak = equity_ticks.front();
        std::int64_t max_drawdown = 0;
        double max_drawdown_ratio = 0.0;

        for (const auto equity : equity_ticks) {
            peak = std::max(peak, equity);
            const auto drawdown = peak - equity;
            if (drawdown > max_drawdown) {
                max_drawdown = drawdown;
                max_drawdown_ratio =
                    peak > 0
                        ? static_cast<double>(drawdown) / static_cast<double>(peak)
                        : 0.0;
            }
        }

        return DrawdownResult{
            max_drawdown,
            max_drawdown_ratio,
            MetricStatus::Ok
        };
    }
};

}  // namespace trading_engine::paper
