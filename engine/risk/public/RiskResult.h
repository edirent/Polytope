#pragma once

#include "engine/risk/metrics/RiskMetrics.h"

#include <cstdint>

namespace trading_engine::risk {

struct RiskStageTimings {
    std::uint64_t total_ns = 0;
    std::uint64_t stage_sum_ns = 0;
    std::uint64_t unattributed_ns = 0;

    std::uint64_t kill_switch_guard_ns = 0;
    std::uint64_t intent_validator_ns = 0;
    std::uint64_t evidence_verifier_ns = 0;
    std::uint64_t expiry_guard_ns = 0;
    std::uint64_t duplicate_guard_ns = 0;
    std::uint64_t rate_limit_guard_ns = 0;
    std::uint64_t market_state_guard_ns = 0;
    std::uint64_t snapshot_freshness_guard_ns = 0;
    std::uint64_t cost_revalidator_ns = 0;
    std::uint64_t vwap_revalidator_ns = 0;
    std::uint64_t edge_guard_ns = 0;
    std::uint64_t max_loss_guard_ns = 0;
    std::uint64_t exposure_guard_ns = 0;
    std::uint64_t inventory_guard_ns = 0;
    std::uint64_t partial_fill_guard_ns = 0;
    std::uint64_t reservation_book_ns = 0;
    std::uint64_t audit_trace_ns = 0;
    std::uint64_t risk_decision_build_ns = 0;
    std::uint64_t publisher_ns = 0;
    std::uint64_t metrics_ns = 0;
};

inline void accumulate_stage_timings(
    RiskStageTimings* lhs,
    const RiskStageTimings& rhs
) noexcept {
    if (lhs == nullptr) {
        return;
    }

    lhs->kill_switch_guard_ns += rhs.kill_switch_guard_ns;
    lhs->total_ns += rhs.total_ns;
    lhs->stage_sum_ns += rhs.stage_sum_ns;
    lhs->unattributed_ns += rhs.unattributed_ns;
    lhs->intent_validator_ns += rhs.intent_validator_ns;
    lhs->evidence_verifier_ns += rhs.evidence_verifier_ns;
    lhs->expiry_guard_ns += rhs.expiry_guard_ns;
    lhs->duplicate_guard_ns += rhs.duplicate_guard_ns;
    lhs->rate_limit_guard_ns += rhs.rate_limit_guard_ns;
    lhs->market_state_guard_ns += rhs.market_state_guard_ns;
    lhs->snapshot_freshness_guard_ns += rhs.snapshot_freshness_guard_ns;
    lhs->cost_revalidator_ns += rhs.cost_revalidator_ns;
    lhs->vwap_revalidator_ns += rhs.vwap_revalidator_ns;
    lhs->edge_guard_ns += rhs.edge_guard_ns;
    lhs->max_loss_guard_ns += rhs.max_loss_guard_ns;
    lhs->exposure_guard_ns += rhs.exposure_guard_ns;
    lhs->inventory_guard_ns += rhs.inventory_guard_ns;
    lhs->partial_fill_guard_ns += rhs.partial_fill_guard_ns;
    lhs->reservation_book_ns += rhs.reservation_book_ns;
    lhs->audit_trace_ns += rhs.audit_trace_ns;
    lhs->risk_decision_build_ns += rhs.risk_decision_build_ns;
    lhs->publisher_ns += rhs.publisher_ns;
    lhs->metrics_ns += rhs.metrics_ns;
}

struct RiskResult {
    std::uint64_t intents_evaluated = 0;
    std::uint64_t intents_approved = 0;
    std::uint64_t intents_rejected = 0;

    std::uint64_t rejected_kill_switch = 0;
    std::uint64_t rejected_low_edge = 0;
    std::uint64_t rejected_cost_limit = 0;
    std::uint64_t rejected_exposure_limit = 0;
    std::uint64_t rejected_inventory_limit = 0;
    std::uint64_t rejected_stale_or_expired = 0;
    std::uint64_t rejected_drift_or_slippage = 0;
    std::uint64_t rejected_pending_or_rate_limit = 0;
    std::uint64_t rejected_bad_market_state = 0;
    std::uint64_t rejected_snapshot_freshness = 0;
    std::uint64_t rejected_partial_fill_risk = 0;

    std::uint64_t output_hash = 0;
    RiskStageTimings stage_timings;
    RiskMetrics metrics;
};

}  // namespace trading_engine::risk
