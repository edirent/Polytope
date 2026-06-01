#include "engine/paper/regime/RegimeEngine.h"

#include <cstddef>

namespace trading_engine::paper {
namespace {

bool is_stale(
    const trading_engine::state::MarketDepthView& view,
    std::uint64_t now_ns,
    std::uint64_t max_age_ns
) noexcept {
    if (view.last_ws_recv_ns == 0) {
        return false;
    }
    return now_ns > view.last_ws_recv_ns &&
           now_ns - view.last_ws_recv_ns > max_age_ns;
}

DataRegime classify_data(
    std::span<const trading_engine::state::MarketDepthView> depth_views,
    std::uint64_t now_ns,
    std::uint64_t max_age_ns
) noexcept {
    for (const auto& view : depth_views) {
        if (is_stale(view, now_ns, max_age_ns)) {
            return DataRegime::Stale;
        }
    }
    return DataRegime::Healthy;
}

LiquidityRegime classify_liquidity(
    std::span<const trading_engine::state::MarketDepthView> depth_views
) noexcept {
    bool degraded = false;
    for (const auto& view : depth_views) {
        if (view.crossed) {
            return LiquidityRegime::Crossed;
        }
        if (!view.usable_for_depth || view.recovering || view.closed ||
            view.resolved || view.bid_count == 0 || view.ask_count == 0) {
            degraded = true;
        }
    }
    return degraded ? LiquidityRegime::Degraded : LiquidityRegime::Healthy;
}

ChainRegime classify_chain(const RegimeInput& input) noexcept {
    if (input.chain_errors_recent > 0) {
        return ChainRegime::Error;
    }
    if (!input.chain_live || input.chain_lagging) {
        return ChainRegime::Lagging;
    }
    return ChainRegime::Healthy;
}

SignalRegime classify_signal(const RegimeInput& input) noexcept {
    if (input.signal_intents_observed == 0) {
        return SignalRegime::Quiet;
    }
    if (input.signal_rejections_observed > input.signal_intents_observed / 2) {
        return SignalRegime::Rejecting;
    }
    return SignalRegime::Healthy;
}

RiskRegime classify_risk(
    std::span<const trading_engine::risk::RiskDecision> decisions,
    const RegimeConfig& config
) noexcept {
    if (decisions.empty()) {
        return RiskRegime::Healthy;
    }

    std::uint64_t rejected = 0;
    for (const auto& decision : decisions) {
        if (decision.reject_reason == trading_engine::risk::RiskRejectReason::KillSwitch) {
            return RiskRegime::KillSwitch;
        }
        if (decision.rejected()) {
            ++rejected;
        }
    }

    if (decisions.size() >= config.min_risk_decisions_for_constrained) {
        const auto reject_rate =
            static_cast<double>(rejected) / static_cast<double>(decisions.size());
        if (reject_rate >= config.constrained_risk_reject_rate) {
            return RiskRegime::Constrained;
        }
    }

    return RiskRegime::Healthy;
}

ExecutionRegime classify_execution(
    std::span<const trading_engine::execution::ExecutionReport> reports,
    std::span<const trading_engine::execution::PlanStatus> plan_statuses
) noexcept {
    for (const auto status : plan_statuses) {
        if (status == trading_engine::execution::PlanStatus::HedgeRequired) {
            return ExecutionRegime::HedgeRequired;
        }
        if (status == trading_engine::execution::PlanStatus::PartiallyFilled) {
            return ExecutionRegime::PartialFill;
        }
    }

    for (const auto& report : reports) {
        if (report.status ==
            trading_engine::execution::ChildOrderStatus::PartiallyFilled) {
            return ExecutionRegime::HedgeRequired;
        }
    }

    return ExecutionRegime::Healthy;
}

}  // namespace

RegimeEngine::RegimeEngine(RegimeConfig config) : config_(config) {}

RegimeSnapshot RegimeEngine::classify(const RegimeInput& input) const {
    RegimeSnapshot snapshot;
    snapshot.ts_ns = input.now_ns;
    snapshot.version = 1;
    snapshot.data = classify_data(
        input.depth_views,
        input.now_ns,
        config_.max_book_age_ns
    );
    snapshot.liquidity = classify_liquidity(input.depth_views);
    snapshot.chain = classify_chain(input);
    snapshot.signal = classify_signal(input);
    snapshot.risk = classify_risk(input.risk_decisions, config_);
    snapshot.execution =
        classify_execution(input.execution_reports, input.plan_statuses);
    return snapshot;
}

}  // namespace trading_engine::paper
