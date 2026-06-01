#pragma once

#include "engine/common/math/VwapMath.h"
#include "engine/decision_fastpath/core/FastPathGate.h"
#include "engine/decision_fastpath/kernel/FixedShapeKernelSpec.h"
#include "engine/execution/public/ChildOrder.h"
#include "engine/execution/public/OrderPlan.h"
#include "engine/risk/ledger/RiskLedger.h"
#include "engine/risk/public/ApprovedIntent.h"
#include "engine/risk/public/RiskDecision.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/view/MarketDepthView.h"

#include <array>
#include <cstdint>

namespace trading_engine::decision_fastpath {

struct FastPathScratch {
    std::array<
        trading_engine::common::math::VwapMathResult,
        kMaxFixedShapeKernelLegs
    > leg_costs{};
    std::uint8_t leg_count = 0;

    std::array<
        trading_engine::signal::IntentLeg,
        kMaxFixedShapeKernelLegs
    > intent_legs{};
    std::array<
        trading_engine::execution::ChildOrder,
        kMaxFixedShapeKernelLegs
    > child_orders{};

    void reset() noexcept;
};

struct FastKernelResult {
    bool produced_intent = false;
    bool produced_plan = false;
    bool fallback_required = false;

    trading_engine::signal::OpportunityIntent intent;
    trading_engine::risk::RiskDecision decision;
    trading_engine::risk::ApprovedIntent approved;
    trading_engine::execution::OrderPlan plan;

    std::int64_t bundle_qty = 0;
    std::int64_t total_cost_tick = 0;
    std::int64_t avg_cost_tick = 0;
    std::int64_t unit_edge_tick = 0;
    std::int64_t total_edge_tick = 0;
    std::int64_t edge_bps = 0;

    std::uint64_t output_hash = 0;

    FastPathRejectReason reject_reason = FastPathRejectReason::None;
    trading_engine::risk::RiskRejectReason risk_reject_reason =
        trading_engine::risk::RiskRejectReason::None;
};

class FixedBuyBundleKernelScalar {
public:
    [[nodiscard]] FastKernelResult run(
        const FixedShapeKernelSpec& spec,
        const trading_engine::state::MarketDepthView* depth_views,
        std::uint16_t depth_view_count,
        const trading_engine::risk::RiskPolicySnapshot& policy,
        const trading_engine::risk::RiskLedgerSnapshot& ledger,
        std::uint64_t now_ns
    ) const;

    [[nodiscard]] FastKernelResult run(
        const FixedShapeKernelSpec& spec,
        const trading_engine::state::MarketDepthView* depth_views,
        std::uint16_t depth_view_count,
        const trading_engine::risk::RiskPolicySnapshot& policy,
        const trading_engine::risk::RiskLedgerSnapshot& ledger,
        std::uint64_t now_ns,
        FastPathScratch* scratch
    ) const;
};

}  // namespace trading_engine::decision_fastpath
