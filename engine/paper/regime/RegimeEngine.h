#pragma once

#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/ExecutionTypes.h"
#include "engine/paper/regime/RegimeSnapshot.h"
#include "engine/risk/public/RiskDecision.h"
#include "engine/state/view/MarketDepthView.h"

#include <cstdint>
#include <span>

namespace trading_engine::paper {

struct RegimeConfig {
    std::uint64_t max_book_age_ns = 1'000'000'000ULL;
    double constrained_risk_reject_rate = 0.5;
    std::uint64_t min_risk_decisions_for_constrained = 1;
};

struct RegimeInput {
    std::uint64_t now_ns = 0;

    std::span<const trading_engine::state::MarketDepthView> depth_views;

    bool chain_live = true;
    bool chain_lagging = false;
    std::uint32_t chain_errors_recent = 0;

    std::uint64_t signal_intents_observed = 0;
    std::uint64_t signal_rejections_observed = 0;

    std::span<const trading_engine::risk::RiskDecision> risk_decisions;

    std::span<const trading_engine::execution::ExecutionReport> execution_reports;
    std::span<const trading_engine::execution::PlanStatus> plan_statuses;
};

class RegimeEngine {
public:
    explicit RegimeEngine(RegimeConfig config = {});

    [[nodiscard]] RegimeSnapshot classify(const RegimeInput& input) const;

private:
    RegimeConfig config_;
};

}  // namespace trading_engine::paper
