#include "engine/paper/metrics/PerformanceMetricsEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace trading_engine::paper {
namespace {

MetricValue ratio_metric(std::uint64_t numerator, std::uint64_t denominator) {
    if (denominator == 0) {
        return {};
    }
    return MetricValue{
        static_cast<double>(numerator) / static_cast<double>(denominator),
        MetricStatus::Ok
    };
}

MetricValue turnover_metric(
    std::int64_t filled_notional_tick,
    std::span<const EquityCurve> equity_curve
) {
    if (equity_curve.empty()) {
        return {};
    }

    double equity_sum = 0.0;
    for (const auto& point : equity_curve) {
        equity_sum += static_cast<double>(point.equity_mid_tick);
    }
    const auto average_equity =
        equity_sum / static_cast<double>(equity_curve.size());
    if (average_equity <= 0.0) {
        return {};
    }

    return MetricValue{
        std::abs(static_cast<double>(filled_notional_tick)) / average_equity,
        MetricStatus::Ok
    };
}

std::vector<double> build_returns(std::span<const EquityCurve> equity_curve) {
    std::vector<double> returns;
    if (equity_curve.size() < 2) {
        return returns;
    }
    returns.reserve(equity_curve.size() - 1);

    for (std::size_t index = 1; index < equity_curve.size(); ++index) {
        const auto previous = equity_curve[index - 1].equity_mid_tick;
        const auto current = equity_curve[index].equity_mid_tick;
        if (previous <= 0) {
            continue;
        }
        returns.push_back(
            static_cast<double>(current - previous) /
            static_cast<double>(previous)
        );
    }

    return returns;
}

MetricValue latest_return_metric(std::span<const double> returns) {
    if (returns.empty()) {
        return {};
    }
    return MetricValue{returns.back(), MetricStatus::Ok};
}

MetricValue volatility_metric(
    std::span<const double> returns,
    std::size_t min_samples
) {
    if (returns.size() < min_samples || returns.size() < 2) {
        return {};
    }

    double sum = 0.0;
    for (const auto value : returns) {
        sum += value;
    }
    const auto mean = sum / static_cast<double>(returns.size());

    double variance_sum = 0.0;
    for (const auto value : returns) {
        const auto diff = value - mean;
        variance_sum += diff * diff;
    }

    return MetricValue{
        std::sqrt(variance_sum / static_cast<double>(returns.size() - 1)),
        MetricStatus::Ok
    };
}

std::vector<std::int64_t> equity_ticks(std::span<const EquityCurve> equity_curve) {
    std::vector<std::int64_t> ticks;
    ticks.reserve(equity_curve.size());
    for (const auto& point : equity_curve) {
        ticks.push_back(point.equity_mid_tick);
    }
    return ticks;
}

}  // namespace

PerformanceMetricsEngine::PerformanceMetricsEngine(
    PerformanceMetricsConfig config
)
    : config_(config) {}

PerformanceMetricsResult PerformanceMetricsEngine::compute(
    const PerformanceMetricsInput& input
) const {
    PerformanceMetricsResult result;

    const auto returns = build_returns(input.equity_curve);
    result.return_samples = returns.size();

    result.latest_return = latest_return_metric(returns);
    result.volatility =
        volatility_metric(returns, config_.min_return_samples_for_sharpe);

    const SharpeCalculator sharpe{
        config_.min_return_samples_for_sharpe,
        config_.sharpe_annualization_factor
    };
    result.sharpe = sharpe.compute(returns);

    const auto ticks = equity_ticks(input.equity_curve);
    result.drawdown = DrawdownCalculator{}.compute(ticks);

    result.fill_rate =
        ratio_metric(input.filled_plans, input.plans_created);
    result.risk_approval_rate =
        ratio_metric(input.approvals_observed, input.intents_observed);
    result.intent_conversion_rate =
        ratio_metric(input.plans_created, input.intents_observed);
    result.turnover =
        turnover_metric(input.filled_notional_tick, input.equity_curve);

    return result;
}

}  // namespace trading_engine::paper
