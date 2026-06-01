#pragma once

#include "engine/paper/metrics/DrawdownCalculator.h"
#include "engine/paper/metrics/SharpeCalculator.h"
#include "engine/paper/pnl/EquityCurve.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace trading_engine::paper {

struct PerformanceMetricsConfig {
    std::size_t min_return_samples_for_sharpe = 2;
    double sharpe_annualization_factor = 1.0;
};

struct PerformanceMetricsInput {
    std::span<const EquityCurve> equity_curve;

    std::uint64_t intents_observed = 0;
    std::uint64_t approvals_observed = 0;
    std::uint64_t plans_created = 0;
    std::uint64_t filled_plans = 0;

    std::int64_t filled_notional_tick = 0;
};

struct PerformanceMetricsResult {
    std::uint64_t return_samples = 0;

    MetricValue latest_return;
    MetricValue volatility;
    MetricValue sharpe;

    DrawdownResult drawdown;

    MetricValue fill_rate;
    MetricValue risk_approval_rate;
    MetricValue intent_conversion_rate;
    MetricValue turnover;
};

class PerformanceMetricsEngine {
public:
    explicit PerformanceMetricsEngine(PerformanceMetricsConfig config = {});

    [[nodiscard]] PerformanceMetricsResult compute(
        const PerformanceMetricsInput& input
    ) const;

private:
    PerformanceMetricsConfig config_;
};

}  // namespace trading_engine::paper
