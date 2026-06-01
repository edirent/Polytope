#pragma once

#include <cstdint>

namespace trading_engine::paper {

enum class PaperMetricStatus : std::uint8_t {
    Ok,
    InsufficientData,
    InvalidInput
};

struct PerformanceSnapshot {
    std::uint64_t intents_observed = 0;
    std::uint64_t approvals_observed = 0;
    std::uint64_t plans_observed = 0;
    std::uint64_t execution_reports_observed = 0;

    std::uint64_t filled_plans = 0;
    std::uint64_t failed_plans = 0;

    std::int64_t gross_pnl_tick = 0;
    std::int64_t net_pnl_tick = 0;
    std::int64_t max_drawdown_tick = 0;
    double max_drawdown_ratio = 0.0;

    std::uint64_t returns_count = 0;
    double latest_return = 0.0;
    PaperMetricStatus latest_return_status =
        PaperMetricStatus::InsufficientData;
    double volatility = 0.0;
    PaperMetricStatus volatility_status =
        PaperMetricStatus::InsufficientData;
    double sharpe = 0.0;
    PaperMetricStatus sharpe_status =
        PaperMetricStatus::InsufficientData;

    double fill_rate = 0.0;
    PaperMetricStatus fill_rate_status =
        PaperMetricStatus::InsufficientData;
    double risk_approval_rate = 0.0;
    PaperMetricStatus risk_approval_rate_status =
        PaperMetricStatus::InsufficientData;
    double intent_conversion_rate = 0.0;
    PaperMetricStatus intent_conversion_rate_status =
        PaperMetricStatus::InsufficientData;
    double turnover = 0.0;
    PaperMetricStatus turnover_status =
        PaperMetricStatus::InsufficientData;

    std::uint64_t version = 0;
    std::uint64_t updated_ts_ns = 0;
};

}  // namespace trading_engine::paper
