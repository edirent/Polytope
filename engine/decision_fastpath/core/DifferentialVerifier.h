#pragma once

#include "engine/decision_fastpath/core/EventLocalDecisionPipeline.h"
#include "engine/decision_fastpath/kernel/FixedBuyBundleKernelScalar.h"
#include "engine/decision_fastpath/kernel/FixedShapeKernelSpec.h"
#include "engine/risk/ledger/RiskLedger.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/state/view/MarketDepthView.h"

#include <cstdint>
#include <span>
#include <string>

namespace trading_engine::decision_fastpath {

struct DecisionPathSnapshot {
    bool produced_intent = false;
    bool produced_plan = false;
    bool fallback_required = false;

    FastPathRejectReason reject_reason = FastPathRejectReason::None;

    trading_engine::signal::IntentStatus intent_status =
        trading_engine::signal::IntentStatus::CandidateOnly;
    trading_engine::risk::RiskRejectReason risk_reject_reason =
        trading_engine::risk::RiskRejectReason::None;

    std::uint64_t bundle_id = 0;

    std::int64_t bundle_qty = 0;
    std::int64_t total_cost_tick = 0;
    std::int64_t unit_edge_tick = 0;
    std::int64_t total_edge_tick = 0;
    std::int64_t edge_bps = 0;

    std::uint16_t order_count = 0;
    std::uint64_t decision_hash = 0;
    std::uint64_t plan_id = 0;

    std::uint64_t opportunity_hash = 0;
    std::uint64_t risk_hash = 0;
    std::uint64_t plan_hash = 0;
    std::uint64_t combined_hash = 0;

    std::uint64_t semantic_hash = 0;
};

struct DifferentialCompareResult {
    bool match = false;

    std::uint64_t fast_opportunity_hash = 0;
    std::uint64_t generic_opportunity_hash = 0;
    std::uint64_t fast_risk_hash = 0;
    std::uint64_t generic_risk_hash = 0;
    std::uint64_t fast_plan_hash = 0;
    std::uint64_t generic_plan_hash = 0;
    std::uint64_t fast_combined_hash = 0;
    std::uint64_t generic_combined_hash = 0;

    std::uint64_t fast_hash = 0;
    std::uint64_t generic_hash = 0;

    std::string first_mismatch;
};

[[nodiscard]] DecisionPathSnapshot snapshot_from_fast_kernel(
    const FastKernelResult& result
) noexcept;

[[nodiscard]] DecisionPathSnapshot snapshot_from_fast_result(
    const FastPathResult& result
) noexcept;

[[nodiscard]] DecisionPathSnapshot reference_generic_decision(
    const FixedShapeKernelSpec& spec,
    std::span<const trading_engine::state::MarketDepthView> depth_views,
    const trading_engine::risk::RiskPolicySnapshot& policy,
    const trading_engine::risk::RiskLedgerSnapshot& ledger,
    std::uint64_t now_ns
) noexcept;

[[nodiscard]] DifferentialCompareResult compare_decision_snapshots(
    const DecisionPathSnapshot& fast,
    const DecisionPathSnapshot& generic
);

}  // namespace trading_engine::decision_fastpath
