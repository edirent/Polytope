#include "engine/decision_fastpath/kernel/FixedBuyBundleKernelScalar.h"

#include "engine/common/math/EdgeMath.h"
#include "engine/common/math/FixedPointMath.h"
#include "engine/common/math/RiskMath.h"
#include "engine/signal/publish/IntentId.h"

#include <algorithm>
#include <limits>

namespace trading_engine::decision_fastpath {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct LegWork {
    const trading_engine::state::MarketDepthView* depth = nullptr;
    std::int64_t ratio_qty_lots = 0;
    std::int64_t executable_qty_lots = 0;
    std::int64_t planned_qty_lots = 0;
    std::int64_t best_price_tick = 0;
    std::int64_t depth_margin_bps = 0;
};

void mix_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
        *hash *= kFnvPrime;
    }
}

void mix_i64(std::uint64_t* hash, std::int64_t value) noexcept {
    mix_u64(hash, static_cast<std::uint64_t>(value));
}

void mix_u8(std::uint64_t* hash, std::uint8_t value) noexcept {
    *hash ^= value;
    *hash *= kFnvPrime;
}

[[nodiscard]] std::uint64_t nonzero_hash(std::uint64_t hash) noexcept {
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] bool checked_add(
    std::int64_t value,
    std::int64_t* total
) noexcept {
    return trading_engine::common::math::checked_add_i64(*total, value, total);
}

[[nodiscard]] bool checked_mul(
    std::int64_t lhs,
    std::int64_t rhs,
    std::int64_t* out
) noexcept {
    return trading_engine::common::math::checked_mul_i64(lhs, rhs, out);
}

[[nodiscard]] std::int64_t saturating_mul(
    std::int64_t lhs,
    std::int64_t rhs
) noexcept {
    std::int64_t out = 0;
    if (!checked_mul(lhs, rhs, &out)) {
        const bool positive = (lhs >= 0 && rhs >= 0) || (lhs < 0 && rhs < 0);
        return positive ? std::numeric_limits<std::int64_t>::max()
                        : std::numeric_limits<std::int64_t>::min();
    }
    return out;
}

[[nodiscard]] FastKernelResult fallback(
    FastPathRejectReason reason
) noexcept {
    FastKernelResult result;
    result.fallback_required = true;
    result.reject_reason = reason;
    return result;
}

[[nodiscard]] const trading_engine::state::MarketDepthView* find_depth(
    const trading_engine::state::MarketDepthView* views,
    std::uint16_t count,
    std::uint32_t asset_index
) noexcept {
    if (views == nullptr) {
        return nullptr;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        if (views[i].asset_index == asset_index) {
            return &views[i];
        }
    }
    return nullptr;
}

[[nodiscard]] bool depth_is_usable(
    const trading_engine::state::MarketDepthView& depth,
    const trading_engine::risk::RiskPolicySnapshot& policy,
    std::uint64_t now_ns
) noexcept {
    if (!depth.usable_for_depth || depth.recovering || depth.crossed ||
        depth.closed || depth.resolved || depth.ask_count == 0) {
        return false;
    }
    if (policy.max_book_age_ns > 0 && now_ns > depth.last_ws_recv_ns &&
        now_ns - depth.last_ws_recv_ns >
            static_cast<std::uint64_t>(policy.max_book_age_ns)) {
        return false;
    }
    return true;
}

[[nodiscard]] std::int64_t executable_ask_qty(
    const trading_engine::state::MarketDepthView& depth
) noexcept {
    if (depth.prefix.ask_count > 0) {
        return trading_engine::state::ask_depth_from_prefix(depth.prefix);
    }

    std::int64_t total = 0;
    const auto count = std::min<std::uint16_t>(
        depth.ask_count,
        trading_engine::state::kMaxSnapshotDepth
    );
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto& level = depth.asks[i];
        if (level.price_tick <= 0) {
            continue;
        }
        if (!checked_add(
                trading_engine::common::math::price_level_size_lots(level),
                &total
            )) {
            return std::numeric_limits<std::int64_t>::max();
        }
    }
    return total;
}

[[nodiscard]] std::int64_t best_ask_tick(
    const trading_engine::state::MarketDepthView& depth
) noexcept {
    if (depth.prefix.ask_count > 0) {
        return trading_engine::state::best_ask_tick_from_prefix(
            depth.asks,
            depth.prefix
        );
    }
    const auto count = std::min<std::uint16_t>(
        depth.ask_count,
        trading_engine::state::kMaxSnapshotDepth
    );
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto& level = depth.asks[i];
        if (level.price_tick > 0 &&
            trading_engine::common::math::price_level_size_lots(level) > 0) {
            return level.price_tick;
        }
    }
    return 0;
}

[[nodiscard]] trading_engine::common::math::VwapMathResult buy_vwap(
    const trading_engine::state::MarketDepthView& depth,
    std::int64_t quantity_lots
) noexcept {
    if (depth.prefix.ask_count > 0) {
        return trading_engine::common::math::buy_vwap_prefix(
            depth.prefix,
            depth.asks.data(),
            depth.ask_count,
            quantity_lots
        );
    }
    return trading_engine::common::math::buy_vwap_linear(
        depth.asks.data(),
        depth.ask_count,
        quantity_lots
    );
}

[[nodiscard]] std::uint64_t snapshot_hash(
    const FixedShapeKernelSpec& spec,
    const std::array<LegWork, kMaxFixedShapeKernelLegs>& legs
) noexcept {
    auto hash = kFnvOffset;
    for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
        const auto* depth = legs[i].depth;
        if (depth == nullptr) {
            continue;
        }
        mix_u64(&hash, depth->asset_index);
        mix_u64(&hash, depth->book_version);
        mix_u64(&hash, depth->snapshot_version_hash);
        mix_u64(&hash, depth->last_ws_recv_ns);
    }
    return nonzero_hash(hash);
}

[[nodiscard]] std::int64_t numeric_asset_lots(
    const trading_engine::risk::RiskLedgerSnapshot& ledger,
    std::uint32_t asset_index
) noexcept {
    for (std::uint8_t i = 0; i < ledger.numeric_asset_count; ++i) {
        const auto& entry = ledger.numeric_reserved_asset_lots[i];
        if (entry.asset_index == asset_index) {
            return entry.lots;
        }
    }
    return 0;
}

[[nodiscard]] std::int64_t numeric_market_exposure(
    const trading_engine::risk::RiskLedgerSnapshot& ledger,
    std::uint32_t market_index
) noexcept {
    for (std::uint8_t i = 0; i < ledger.numeric_market_count; ++i) {
        const auto& entry = ledger.numeric_reserved_market_exposure[i];
        if (entry.market_index == market_index) {
            return entry.exposure_tick;
        }
    }
    return 0;
}

[[nodiscard]] trading_engine::risk::RiskRejectReason risk_reject_reason(
    trading_engine::risk::RiskDecisionType type
) noexcept {
    using trading_engine::risk::RiskDecisionType;
    using trading_engine::risk::RiskRejectReason;
    switch (type) {
        case RiskDecisionType::RejectLowTotalEdge:
            return RiskRejectReason::LowTotalEdge;
        case RiskDecisionType::RejectLowUnitEdge:
            return RiskRejectReason::LowUnitEdge;
        case RiskDecisionType::RejectLowEdgeBps:
            return RiskRejectReason::LowEdgeBps;
        case RiskDecisionType::RejectCostLimit:
            return RiskRejectReason::CostLimit;
        case RiskDecisionType::RejectTotalExposureLimit:
            return RiskRejectReason::TotalExposureLimit;
        case RiskDecisionType::RejectSingleMarketExposureLimit:
            return RiskRejectReason::SingleMarketExposureLimit;
        case RiskDecisionType::RejectInventoryLimit:
            return RiskRejectReason::InventoryLimit;
        case RiskDecisionType::RejectInsufficientDepth:
            return RiskRejectReason::PartialFillRisk;
        case RiskDecisionType::RejectInternalError:
            return RiskRejectReason::InternalError;
        default:
            break;
    }
    return RiskRejectReason::InternalError;
}

[[nodiscard]] trading_engine::risk::RiskDecisionType edge_rejection(
    const FixedShapeKernelSpec& spec,
    const trading_engine::common::math::EdgeMathResult& edge,
    std::int64_t bundle_qty,
    const trading_engine::risk::RiskPolicySnapshot& policy
) noexcept {
    if (!edge.ok) {
        return trading_engine::risk::RiskDecisionType::RejectInternalError;
    }
    const auto min_unit = std::max(
        spec.min_unit_edge_tick,
        policy.min_post_risk_unit_edge_tick
    );
    const auto min_total = std::max(
        spec.min_total_edge_tick,
        policy.min_post_risk_total_edge_tick
    );
    const auto min_bps = std::max(spec.min_edge_bps, policy.min_edge_bps);
    const auto min_qty = std::max<std::int64_t>(spec.min_bundle_qty, 1);

    if (bundle_qty < min_qty) {
        return trading_engine::risk::RiskDecisionType::RejectInsufficientDepth;
    }
    if (edge.unit_edge_tick < min_unit) {
        return trading_engine::risk::RiskDecisionType::RejectLowUnitEdge;
    }
    if (edge.total_edge_tick < min_total) {
        return trading_engine::risk::RiskDecisionType::RejectLowTotalEdge;
    }
    if (edge.edge_bps < min_bps) {
        return trading_engine::risk::RiskDecisionType::RejectLowEdgeBps;
    }
    if (!trading_engine::common::math::passes_edge_thresholds(
            edge.unit_edge_tick,
            edge.total_edge_tick,
            edge.edge_bps,
            policy
        )) {
        return trading_engine::risk::RiskDecisionType::RejectLowTotalEdge;
    }
    return trading_engine::risk::RiskDecisionType::Approve;
}

[[nodiscard]] trading_engine::risk::RiskDecisionType risk_limit_rejection(
    const FixedShapeKernelSpec& spec,
    const std::array<LegWork, kMaxFixedShapeKernelLegs>& legs,
    const FastPathScratch& scratch,
    std::int64_t total_cost_tick,
    const trading_engine::risk::RiskPolicySnapshot& policy,
    const trading_engine::risk::RiskLedgerSnapshot& ledger
) noexcept {
    if (policy.max_total_cost_tick > 0 &&
        total_cost_tick > policy.max_total_cost_tick) {
        return trading_engine::risk::RiskDecisionType::RejectCostLimit;
    }
    if (policy.max_total_exposure_tick > 0 &&
        ledger.total_reserved_exposure_tick + total_cost_tick >
            policy.max_total_exposure_tick) {
        return trading_engine::risk::RiskDecisionType::RejectTotalExposureLimit;
    }
    if (policy.max_single_market_exposure_tick > 0) {
        std::array<trading_engine::risk::NumericReservedMarketExposure, kMaxFixedShapeKernelLegs>
            incoming{};
        std::uint8_t count = 0;
        for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
            bool found = false;
            for (std::uint8_t j = 0; j < count; ++j) {
                if (incoming[j].market_index == spec.market_indices[i]) {
                    incoming[j].exposure_tick +=
                        std::max<std::int64_t>(
                            0,
                            scratch.leg_costs[i].total_cost_tick
                        );
                    found = true;
                    break;
                }
            }
            if (!found && count < incoming.size()) {
                incoming[count++] = {
                    .market_index = spec.market_indices[i],
                    .exposure_tick = std::max<std::int64_t>(
                        0,
                        scratch.leg_costs[i].total_cost_tick
                    )
                };
            }
        }

        for (std::uint8_t i = 0; i < count; ++i) {
            const auto current =
                numeric_market_exposure(ledger, incoming[i].market_index);
            if (current + incoming[i].exposure_tick >
                policy.max_single_market_exposure_tick) {
                return trading_engine::risk::RiskDecisionType::
                    RejectSingleMarketExposureLimit;
            }
        }
    }
    if (policy.max_inventory_lots_per_asset > 0) {
        std::array<trading_engine::risk::NumericReservedAssetLot, kMaxFixedShapeKernelLegs>
            incoming{};
        std::uint8_t count = 0;
        for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
            bool found = false;
            for (std::uint8_t j = 0; j < count; ++j) {
                if (incoming[j].asset_index == spec.asset_indices[i]) {
                    incoming[j].lots += legs[i].planned_qty_lots;
                    found = true;
                    break;
                }
            }
            if (!found && count < incoming.size()) {
                incoming[count++] = {
                    .asset_index = spec.asset_indices[i],
                    .lots = legs[i].planned_qty_lots
                };
            }
        }

        for (std::uint8_t i = 0; i < count; ++i) {
            const auto current =
                numeric_asset_lots(ledger, incoming[i].asset_index);
            if (current + incoming[i].lots >
                policy.max_inventory_lots_per_asset) {
                return trading_engine::risk::RiskDecisionType::
                    RejectInventoryLimit;
            }
        }
    }
    return trading_engine::risk::RiskDecisionType::Approve;
}

[[nodiscard]] std::uint64_t make_decision_id(
    const trading_engine::signal::OpportunityIntent& intent,
    const trading_engine::risk::RiskPolicySnapshot& policy,
    trading_engine::risk::RiskDecisionStatus status,
    trading_engine::risk::RiskRejectReason reason
) noexcept {
    auto hash = kFnvOffset;
    mix_u64(&hash, intent.intent_id);
    mix_u64(&hash, intent.bundle_id);
    mix_u64(&hash, policy.policy_version);
    mix_u64(&hash, policy.policy_hash);
    mix_u8(&hash, static_cast<std::uint8_t>(status));
    mix_u8(&hash, static_cast<std::uint8_t>(reason));
    return nonzero_hash(hash);
}

void fill_decision(
    const trading_engine::signal::OpportunityIntent& intent,
    const trading_engine::risk::RiskPolicySnapshot& policy,
    trading_engine::risk::RiskRejectReason reason,
    trading_engine::risk::RiskDecision* out
) noexcept {
    out->status = reason == trading_engine::risk::RiskRejectReason::None
        ? trading_engine::risk::RiskDecisionStatus::Approved
        : trading_engine::risk::RiskDecisionStatus::Rejected;
    out->reject_reason = reason;
    out->decision_id = make_decision_id(intent, policy, out->status, reason);
    out->intent_id = intent.intent_id;
    out->bundle_id = intent.bundle_id;
    out->idempotency_hash = intent.idempotency_hash;
    out->oracle_artifact_hash = intent.oracle_artifact_hash;
    out->constraint_hash = intent.constraint_hash;
    out->bundle_hash = intent.bundle_hash;
    out->snapshot_version_hash = intent.snapshot_version_hash;
    out->policy_version = policy.policy_version;
    out->policy_hash = policy.policy_hash;
}

[[nodiscard]] std::uint64_t make_reservation_id(
    const trading_engine::signal::OpportunityIntent& intent,
    const trading_engine::risk::RiskDecision& decision
) noexcept {
    auto hash = kFnvOffset;
    mix_u64(&hash, intent.intent_id);
    mix_u64(&hash, decision.decision_id);
    mix_u64(&hash, intent.idempotency_hash);
    return nonzero_hash(hash);
}

[[nodiscard]] std::uint64_t make_plan_id(
    const trading_engine::signal::OpportunityIntent& intent,
    const trading_engine::risk::RiskDecision& decision,
    std::uint64_t reservation_id
) noexcept {
    auto hash = kFnvOffset;
    mix_u64(&hash, intent.intent_id);
    mix_u64(&hash, decision.decision_id);
    mix_u64(&hash, reservation_id);
    mix_u64(&hash, intent.bundle_hash);
    return nonzero_hash(hash);
}

void fill_intent(
    const FixedShapeKernelSpec& spec,
    const std::array<LegWork, kMaxFixedShapeKernelLegs>& legs,
    const FastPathScratch& scratch,
    const trading_engine::common::math::EdgeMathResult& edge,
    std::int64_t bundle_qty,
    std::int64_t total_cost_tick,
    std::int64_t avg_cost_tick,
    std::int64_t max_leg_slippage_tick,
    std::uint64_t snapshot_version,
    std::uint64_t snapshot_version_hash,
    std::uint64_t now_ns,
    bool paper_opportunity,
    trading_engine::signal::OpportunityIntent* out
) noexcept {
    out->bundle_id = spec.bundle_id;
    out->status = paper_opportunity
        ? trading_engine::signal::IntentStatus::PaperOpportunity
        : trading_engine::signal::IntentStatus::RejectedLowEdge;
    out->reject_code = paper_opportunity
        ? trading_engine::signal::IntentRejectCode::None
        : trading_engine::signal::IntentRejectCode::LowEdge;
    out->valid_under_settlement = true;
    out->passed_quality_gate = true;
    out->enough_depth = true;
    out->oracle_artifact_hash = spec.artifact_hash;
    out->constraint_hash = spec.constraint_hash;
    out->bundle_hash = spec.bundle_hash;
    out->snapshot_version = snapshot_version;
    out->snapshot_version_hash = snapshot_version_hash;
    out->bundle_qty = bundle_qty;
    out->original_bundle_qty = bundle_qty;
    out->guaranteed_payout_tick =
        saturating_mul(spec.guaranteed_payout_tick, bundle_qty);
    out->estimated_cost_tick = total_cost_tick;
    out->estimated_fee_tick = 0;
    out->latency_buffer_tick = 0;
    out->slippage_buffer_tick = 0;
    out->max_leg_slippage_tick = max_leg_slippage_tick;
    out->unit_edge_tick = edge.unit_edge_tick;
    out->total_edge_tick = edge.total_edge_tick;
    out->estimated_edge_tick = edge.total_edge_tick;
    out->edge_bps = edge.edge_bps;
    out->min_edge_tick = spec.min_unit_edge_tick;
    out->created_ts_ns = now_ns;
    out->expires_at_ns = now_ns + 1'000'000'000ULL;
    out->leg_count = spec.leg_count;

    for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
        auto& leg = out->legs[i];
        leg.asset_index = spec.asset_indices[i];
        leg.side = spec.sides[i];
        leg.quantity_lots = spec.ratio_qty_lots[i];
        leg.estimated_vwap_tick = scratch.leg_costs[i].vwap_tick;
        leg.worst_price_tick = scratch.leg_costs[i].worst_price_tick;
        leg.estimated_cost_tick = scratch.leg_costs[i].total_cost_tick;
        leg.requested_qty_lots = legs[i].planned_qty_lots;
        leg.executable_qty_lots = legs[i].executable_qty_lots;
        leg.depth_margin_bps = legs[i].depth_margin_bps;
        leg.enough_depth = true;
    }

    const trading_engine::signal::IntentIdentityInput identity{
        .bundle_id = out->bundle_id,
        .bundle_hash = out->bundle_hash,
        .snapshot_version_hash = out->snapshot_version_hash,
        .bundle_qty = out->bundle_qty,
        .unit_edge_tick = out->unit_edge_tick
    };
    out->intent_id = trading_engine::signal::make_intent_id(identity);
    out->idempotency_hash = out->intent_id;

    auto proof_hash = kFnvOffset;
    mix_u64(&proof_hash, out->oracle_artifact_hash);
    mix_u64(&proof_hash, out->constraint_hash);
    mix_u64(&proof_hash, out->bundle_hash);
    mix_u64(&proof_hash, out->snapshot_version_hash);
    out->proof_hash = nonzero_hash(proof_hash);
    (void)avg_cost_tick;
}

void fill_plan(
    const trading_engine::signal::OpportunityIntent& intent,
    const trading_engine::risk::RiskDecision& decision,
    std::uint64_t reservation_id,
    trading_engine::execution::OrderPlan* out
) noexcept {
    out->plan_id = make_plan_id(intent, decision, reservation_id);
    out->source_intent_id = intent.intent_id;
    out->approved_intent_id = decision.decision_id;
    out->reservation_id = reservation_id;
    out->bundle_id = intent.bundle_id;
    out->order_count = intent.leg_count;
    out->max_total_cost_tick = intent.estimated_cost_tick;
    out->min_expected_edge_tick = intent.total_edge_tick;
    out->max_slippage_tick = intent.slippage_buffer_tick;
    out->created_ts_ns = intent.created_ts_ns;
    out->expire_after_ns = intent.expires_at_ns;

    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto& leg = intent.legs[i];
        auto& order = out->orders[i];
        order.order_id =
            static_cast<trading_engine::execution::ChildOrderId>(i + 1U);
        order.plan_id = out->plan_id;
        order.side = trading_engine::execution::OrderSide::Buy;
        order.quantity_lots = leg.requested_qty_lots;
        order.limit_price_tick = leg.worst_price_tick;
        order.estimated_vwap_tick = leg.estimated_vwap_tick;
        order.worst_allowed_price_tick = leg.worst_price_tick;
    }
}

[[nodiscard]] std::uint64_t make_output_hash(
    const FastKernelResult& result
) noexcept {
    auto hash = kFnvOffset;
    mix_u8(&hash, result.produced_intent ? 1U : 0U);
    mix_u8(&hash, result.produced_plan ? 1U : 0U);
    mix_u8(&hash, result.fallback_required ? 1U : 0U);
    mix_u8(&hash, static_cast<std::uint8_t>(result.reject_reason));
    mix_u8(&hash, static_cast<std::uint8_t>(result.risk_reject_reason));
    mix_u64(&hash, result.intent.intent_id);
    mix_u64(&hash, result.intent.bundle_id);
    mix_u64(&hash, result.intent.snapshot_version_hash);
    mix_i64(&hash, result.bundle_qty);
    mix_i64(&hash, result.total_cost_tick);
    mix_i64(&hash, result.unit_edge_tick);
    mix_i64(&hash, result.total_edge_tick);
    mix_i64(&hash, result.edge_bps);
    mix_u64(&hash, result.decision.decision_id);
    mix_u64(&hash, result.plan.plan_id);
    mix_u64(&hash, result.plan.reservation_id);
    return nonzero_hash(hash);
}

}  // namespace

void FastPathScratch::reset() noexcept {
    leg_count = 0;
    leg_costs = {};
    intent_legs = {};
    child_orders = {};
}

FastKernelResult FixedBuyBundleKernelScalar::run(
    const FixedShapeKernelSpec& spec,
    const trading_engine::state::MarketDepthView* depth_views,
    std::uint16_t depth_view_count,
    const trading_engine::risk::RiskPolicySnapshot& policy,
    const trading_engine::risk::RiskLedgerSnapshot& ledger,
    std::uint64_t now_ns
) const {
    FastPathScratch scratch;
    return run(
        spec,
        depth_views,
        depth_view_count,
        policy,
        ledger,
        now_ns,
        &scratch
    );
}

FastKernelResult FixedBuyBundleKernelScalar::run(
    const FixedShapeKernelSpec& spec,
    const trading_engine::state::MarketDepthView* depth_views,
    std::uint16_t depth_view_count,
    const trading_engine::risk::RiskPolicySnapshot& policy,
    const trading_engine::risk::RiskLedgerSnapshot& ledger,
    std::uint64_t now_ns,
    FastPathScratch* scratch
) const {
    if (scratch == nullptr) {
        return fallback(FastPathRejectReason::MissingDepthView);
    }
    scratch->reset();

    if (spec.leg_count == 0) {
        return fallback(FastPathRejectReason::EmptyBundle);
    }
    if (spec.leg_count > kMaxFixedShapeKernelLegs) {
        return fallback(FastPathRejectReason::TooManyLegs);
    }
    if (!policy.risk_enabled || policy.kill_switch_enabled) {
        return fallback(FastPathRejectReason::PolicyIncompatible);
    }

    std::array<LegWork, kMaxFixedShapeKernelLegs> legs{};
    std::int64_t bundle_qty = std::numeric_limits<std::int64_t>::max();
    std::uint64_t snapshot_version = 0;

    for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
        if (spec.sides[i] != trading_engine::oracle::Side::Buy) {
            return fallback(FastPathRejectReason::SellLeg);
        }
        auto& leg = legs[i];
        leg.ratio_qty_lots = spec.ratio_qty_lots[i];
        if (leg.ratio_qty_lots <= 0) {
            return fallback(FastPathRejectReason::UnsupportedExecutableSide);
        }
        leg.depth = find_depth(depth_views, depth_view_count, spec.asset_indices[i]);
        if (leg.depth == nullptr) {
            return fallback(FastPathRejectReason::MissingDepthView);
        }
        if (!depth_is_usable(*leg.depth, policy, now_ns)) {
            return fallback(FastPathRejectReason::BadDepthView);
        }

        leg.executable_qty_lots = executable_ask_qty(*leg.depth);
        leg.best_price_tick = best_ask_tick(*leg.depth);
        if (leg.executable_qty_lots < leg.ratio_qty_lots ||
            leg.best_price_tick <= 0) {
            return fallback(FastPathRejectReason::BadDepthView);
        }
        bundle_qty = std::min(
            bundle_qty,
            leg.executable_qty_lots / leg.ratio_qty_lots
        );
        snapshot_version = std::max(snapshot_version, leg.depth->book_version);
    }

    if (bundle_qty <= 0 ||
        bundle_qty == std::numeric_limits<std::int64_t>::max()) {
        return fallback(FastPathRejectReason::BadDepthView);
    }

    std::int64_t total_cost_tick = 0;
    std::int64_t max_leg_slippage_tick = 0;
    for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
        auto& leg = legs[i];
        if (!checked_mul(leg.ratio_qty_lots, bundle_qty, &leg.planned_qty_lots)) {
            return fallback(FastPathRejectReason::UnsupportedExecutableSide);
        }
        scratch->leg_costs[i] = buy_vwap(*leg.depth, leg.planned_qty_lots);
        if (!scratch->leg_costs[i].ok) {
            return fallback(FastPathRejectReason::BadDepthView);
        }
        leg.depth_margin_bps = trading_engine::common::math::ratio_bps(
            leg.executable_qty_lots,
            leg.planned_qty_lots
        );
        max_leg_slippage_tick = std::max(
            max_leg_slippage_tick,
            scratch->leg_costs[i].worst_price_tick - leg.best_price_tick
        );
        if (!checked_add(scratch->leg_costs[i].total_cost_tick, &total_cost_tick)) {
            return fallback(FastPathRejectReason::UnsupportedExecutableSide);
        }
    }
    scratch->leg_count = spec.leg_count;

    const auto avg_cost_tick =
        trading_engine::common::math::ceil_div_positive(
            total_cost_tick,
            bundle_qty
        );
    const auto edge = trading_engine::common::math::compute_edge({
        .guaranteed_payout_tick = spec.guaranteed_payout_tick,
        .total_cost_tick = avg_cost_tick,
        .fee_tick = 0,
        .latency_buffer_tick = 0,
        .slippage_buffer_tick = 0,
        .bundle_qty = bundle_qty
    });

    FastKernelResult result;
    result.produced_intent = true;
    result.bundle_qty = bundle_qty;
    result.total_cost_tick = total_cost_tick;
    result.avg_cost_tick = avg_cost_tick;
    result.unit_edge_tick = edge.unit_edge_tick;
    result.total_edge_tick = edge.total_edge_tick;
    result.edge_bps = edge.edge_bps;

    auto rejection = edge_rejection(spec, edge, bundle_qty, policy);
    if (rejection == trading_engine::risk::RiskDecisionType::Approve) {
        rejection = risk_limit_rejection(
            spec,
            legs,
            *scratch,
            total_cost_tick,
            policy,
            ledger
        );
    }

    const auto risk_reason = rejection ==
            trading_engine::risk::RiskDecisionType::Approve
        ? trading_engine::risk::RiskRejectReason::None
        : risk_reject_reason(rejection);
    result.risk_reject_reason = risk_reason;

    fill_intent(
        spec,
        legs,
        *scratch,
        edge,
        bundle_qty,
        total_cost_tick,
        avg_cost_tick,
        max_leg_slippage_tick,
        snapshot_version,
        snapshot_hash(spec, legs),
        now_ns,
        risk_reason == trading_engine::risk::RiskRejectReason::None,
        &result.intent
    );
    fill_decision(result.intent, policy, risk_reason, &result.decision);

    if (risk_reason == trading_engine::risk::RiskRejectReason::None) {
        const auto reservation_id =
            make_reservation_id(result.intent, result.decision);
        fill_plan(result.intent, result.decision, reservation_id, &result.plan);
        result.approved.intent = result.intent;
        result.approved.decision = result.decision;
        result.produced_plan = true;
    }

    result.output_hash = make_output_hash(result);
    return result;
}

}  // namespace trading_engine::decision_fastpath
