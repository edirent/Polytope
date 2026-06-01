#include "engine/decision_fastpath/core/DifferentialVerifier.h"

#include "engine/common/math/EdgeMath.h"
#include "engine/common/math/FixedPointMath.h"
#include "engine/common/math/RiskMath.h"
#include "engine/common/math/VwapMath.h"

#include <algorithm>
#include <array>
#include <limits>

namespace trading_engine::decision_fastpath {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct GenericLegWork {
    const trading_engine::state::MarketDepthView* depth = nullptr;
    std::int64_t ratio_qty_lots = 0;
    std::int64_t executable_qty_lots = 0;
    std::int64_t planned_qty_lots = 0;
    trading_engine::common::math::VwapMathResult vwap;
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

[[nodiscard]] std::uint64_t semantic_hash(
    const DecisionPathSnapshot& snapshot
) noexcept {
    auto hash = kFnvOffset;
    mix_u8(&hash, snapshot.produced_intent ? 1U : 0U);
    mix_u8(&hash, snapshot.produced_plan ? 1U : 0U);
    mix_u8(&hash, snapshot.fallback_required ? 1U : 0U);
    mix_u8(&hash, static_cast<std::uint8_t>(snapshot.reject_reason));
    mix_u8(&hash, static_cast<std::uint8_t>(snapshot.intent_status));
    mix_u8(&hash, static_cast<std::uint8_t>(snapshot.risk_reject_reason));
    mix_u64(&hash, snapshot.bundle_id);
    mix_i64(&hash, snapshot.bundle_qty);
    mix_i64(&hash, snapshot.total_cost_tick);
    mix_i64(&hash, snapshot.unit_edge_tick);
    mix_i64(&hash, snapshot.total_edge_tick);
    mix_i64(&hash, snapshot.edge_bps);
    mix_u64(&hash, snapshot.order_count);
    return nonzero_hash(hash);
}

[[nodiscard]] std::uint64_t opportunity_hash(
    const DecisionPathSnapshot& snapshot
) noexcept {
    auto hash = kFnvOffset;
    mix_u8(&hash, 1U);
    mix_u8(&hash, snapshot.produced_intent ? 1U : 0U);
    mix_u8(&hash, snapshot.fallback_required ? 1U : 0U);
    mix_u8(&hash, static_cast<std::uint8_t>(snapshot.reject_reason));
    mix_u8(&hash, static_cast<std::uint8_t>(snapshot.intent_status));
    mix_u64(&hash, snapshot.bundle_id);
    mix_i64(&hash, snapshot.bundle_qty);
    mix_i64(&hash, snapshot.total_cost_tick);
    mix_i64(&hash, snapshot.unit_edge_tick);
    mix_i64(&hash, snapshot.total_edge_tick);
    mix_i64(&hash, snapshot.edge_bps);
    return nonzero_hash(hash);
}

[[nodiscard]] std::uint64_t risk_hash(
    const DecisionPathSnapshot& snapshot
) noexcept {
    auto hash = kFnvOffset;
    mix_u8(&hash, 2U);
    mix_u8(&hash, snapshot.produced_intent ? 1U : 0U);
    mix_u8(&hash, snapshot.produced_plan ? 1U : 0U);
    mix_u8(&hash, static_cast<std::uint8_t>(snapshot.risk_reject_reason));
    mix_u64(&hash, snapshot.bundle_id);
    mix_i64(&hash, snapshot.bundle_qty);
    mix_i64(&hash, snapshot.total_edge_tick);
    mix_i64(&hash, snapshot.edge_bps);
    return nonzero_hash(hash);
}

[[nodiscard]] std::uint64_t plan_hash(
    const DecisionPathSnapshot& snapshot
) noexcept {
    auto hash = kFnvOffset;
    mix_u8(&hash, 3U);
    mix_u8(&hash, snapshot.produced_plan ? 1U : 0U);
    mix_u64(&hash, snapshot.bundle_id);
    mix_u64(&hash, snapshot.order_count);
    return nonzero_hash(hash);
}

[[nodiscard]] std::uint64_t combined_hash(
    const DecisionPathSnapshot& snapshot
) noexcept {
    auto hash = kFnvOffset;
    mix_u8(&hash, 4U);
    mix_u64(&hash, snapshot.opportunity_hash);
    mix_u64(&hash, snapshot.risk_hash);
    mix_u64(&hash, snapshot.plan_hash);
    return nonzero_hash(hash);
}

void finalize(DecisionPathSnapshot* snapshot) noexcept {
    snapshot->opportunity_hash = opportunity_hash(*snapshot);
    snapshot->risk_hash = risk_hash(*snapshot);
    snapshot->plan_hash = plan_hash(*snapshot);
    snapshot->combined_hash = combined_hash(*snapshot);
    snapshot->semantic_hash = semantic_hash(*snapshot);
}

[[nodiscard]] const trading_engine::state::MarketDepthView* find_depth(
    std::span<const trading_engine::state::MarketDepthView> views,
    std::uint32_t asset_index
) noexcept {
    for (const auto& view : views) {
        if (view.asset_index == asset_index) {
            return &view;
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
        if (!trading_engine::common::math::checked_add_i64(
                total,
                trading_engine::common::math::price_level_size_lots(level),
                &total
            )) {
            return std::numeric_limits<std::int64_t>::max();
        }
    }
    return total;
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
    const std::array<GenericLegWork, kMaxFixedShapeKernelLegs>& legs,
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
        std::array<
            trading_engine::risk::NumericReservedMarketExposure,
            kMaxFixedShapeKernelLegs
        > incoming{};
        std::uint8_t count = 0;
        for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
            bool found = false;
            for (std::uint8_t j = 0; j < count; ++j) {
                if (incoming[j].market_index == spec.market_indices[i]) {
                    incoming[j].exposure_tick +=
                        std::max<std::int64_t>(0, legs[i].vwap.total_cost_tick);
                    found = true;
                    break;
                }
            }
            if (!found && count < incoming.size()) {
                incoming[count++] = {
                    .market_index = spec.market_indices[i],
                    .exposure_tick =
                        std::max<std::int64_t>(0, legs[i].vwap.total_cost_tick)
                };
            }
        }
        for (std::uint8_t i = 0; i < count; ++i) {
            if (numeric_market_exposure(ledger, incoming[i].market_index) +
                    incoming[i].exposure_tick >
                policy.max_single_market_exposure_tick) {
                return trading_engine::risk::RiskDecisionType::
                    RejectSingleMarketExposureLimit;
            }
        }
    }
    if (policy.max_inventory_lots_per_asset > 0) {
        std::array<
            trading_engine::risk::NumericReservedAssetLot,
            kMaxFixedShapeKernelLegs
        > incoming{};
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
            if (numeric_asset_lots(ledger, incoming[i].asset_index) +
                    incoming[i].lots >
                policy.max_inventory_lots_per_asset) {
                return trading_engine::risk::RiskDecisionType::
                    RejectInventoryLimit;
            }
        }
    }
    return trading_engine::risk::RiskDecisionType::Approve;
}

void record_mismatch(
    const char* field,
    DifferentialCompareResult* result
) {
    if (result->first_mismatch.empty()) {
        result->first_mismatch = field;
    }
    result->match = false;
}

template <typename T>
void compare_field(
    const char* field,
    const T& lhs,
    const T& rhs,
    DifferentialCompareResult* result
) {
    if (!(lhs == rhs)) {
        record_mismatch(field, result);
    }
}

}  // namespace

DecisionPathSnapshot snapshot_from_fast_kernel(
    const FastKernelResult& result
) noexcept {
    DecisionPathSnapshot snapshot;
    snapshot.produced_intent = result.produced_intent;
    snapshot.produced_plan = result.produced_plan;
    snapshot.fallback_required = result.fallback_required;
    snapshot.reject_reason = result.reject_reason;
    snapshot.intent_status = result.intent.status;
    snapshot.risk_reject_reason = result.risk_reject_reason;
    snapshot.bundle_id = result.intent.bundle_id;
    snapshot.bundle_qty = result.bundle_qty;
    snapshot.total_cost_tick = result.total_cost_tick;
    snapshot.unit_edge_tick = result.unit_edge_tick;
    snapshot.total_edge_tick = result.total_edge_tick;
    snapshot.edge_bps = result.edge_bps;
    snapshot.order_count = result.produced_plan
        ? result.plan.order_count
        : result.order_decision.leg_count;
    snapshot.decision_hash = result.order_decision.decision_hash;
    snapshot.plan_id = result.plan.plan_id;
    finalize(&snapshot);
    return snapshot;
}

DecisionPathSnapshot snapshot_from_fast_result(
    const FastPathResult& result
) noexcept {
    DecisionPathSnapshot snapshot;
    snapshot.produced_intent =
        result.intent.intent_id != 0 || result.intent.bundle_id != 0;
    snapshot.produced_plan = result.produced_plan;
    snapshot.fallback_required = result.fallback_required;
    snapshot.reject_reason = result.reject_reason;
    snapshot.intent_status = result.intent.status;
    snapshot.risk_reject_reason = result.fallback_required
        ? trading_engine::risk::RiskRejectReason::None
        : result.decision.reject_reason;
    snapshot.bundle_id = result.intent.bundle_id;
    snapshot.bundle_qty = result.intent.bundle_qty;
    snapshot.total_cost_tick = result.intent.estimated_cost_tick;
    snapshot.unit_edge_tick = result.intent.unit_edge_tick;
    snapshot.total_edge_tick = result.intent.total_edge_tick;
    snapshot.edge_bps = result.intent.edge_bps;
    snapshot.order_count = result.plan.order_count;
    snapshot.decision_hash = result.order_decision.decision_hash;
    snapshot.plan_id = result.plan.plan_id;
    finalize(&snapshot);
    return snapshot;
}

DecisionPathSnapshot reference_generic_decision(
    const FixedShapeKernelSpec& spec,
    std::span<const trading_engine::state::MarketDepthView> depth_views,
    const trading_engine::risk::RiskPolicySnapshot& policy,
    const trading_engine::risk::RiskLedgerSnapshot& ledger,
    std::uint64_t now_ns
) noexcept {
    DecisionPathSnapshot snapshot;

    if (spec.leg_count == 0) {
        snapshot.fallback_required = true;
        snapshot.reject_reason = FastPathRejectReason::EmptyBundle;
        finalize(&snapshot);
        return snapshot;
    }
    if (spec.leg_count > kMaxFixedShapeKernelLegs) {
        snapshot.fallback_required = true;
        snapshot.reject_reason = FastPathRejectReason::TooManyLegs;
        finalize(&snapshot);
        return snapshot;
    }
    if (!policy.risk_enabled || policy.kill_switch_enabled) {
        snapshot.fallback_required = true;
        snapshot.reject_reason = FastPathRejectReason::PolicyIncompatible;
        finalize(&snapshot);
        return snapshot;
    }

    std::array<GenericLegWork, kMaxFixedShapeKernelLegs> legs{};
    std::int64_t bundle_qty = std::numeric_limits<std::int64_t>::max();
    for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
        if (spec.sides[i] != trading_engine::oracle::Side::Buy) {
            snapshot.fallback_required = true;
            snapshot.reject_reason = FastPathRejectReason::SellLeg;
            finalize(&snapshot);
            return snapshot;
        }
        auto& leg = legs[i];
        leg.ratio_qty_lots = spec.ratio_qty_lots[i];
        if (leg.ratio_qty_lots <= 0) {
            snapshot.fallback_required = true;
            snapshot.reject_reason = FastPathRejectReason::UnsupportedExecutableSide;
            finalize(&snapshot);
            return snapshot;
        }
        leg.depth = find_depth(depth_views, spec.asset_indices[i]);
        if (leg.depth == nullptr) {
            snapshot.fallback_required = true;
            snapshot.reject_reason = FastPathRejectReason::MissingDepthView;
            finalize(&snapshot);
            return snapshot;
        }
        if (!depth_is_usable(*leg.depth, policy, now_ns)) {
            snapshot.fallback_required = true;
            snapshot.reject_reason = FastPathRejectReason::BadDepthView;
            finalize(&snapshot);
            return snapshot;
        }
        leg.executable_qty_lots = executable_ask_qty(*leg.depth);
        if (leg.executable_qty_lots < leg.ratio_qty_lots) {
            snapshot.fallback_required = true;
            snapshot.reject_reason = FastPathRejectReason::BadDepthView;
            finalize(&snapshot);
            return snapshot;
        }
        bundle_qty = std::min(
            bundle_qty,
            leg.executable_qty_lots / leg.ratio_qty_lots
        );
    }

    if (bundle_qty <= 0 ||
        bundle_qty == std::numeric_limits<std::int64_t>::max()) {
        snapshot.fallback_required = true;
        snapshot.reject_reason = FastPathRejectReason::BadDepthView;
        finalize(&snapshot);
        return snapshot;
    }

    std::int64_t total_cost_tick = 0;
    for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
        auto& leg = legs[i];
        if (!trading_engine::common::math::checked_mul_i64(
                leg.ratio_qty_lots,
                bundle_qty,
                &leg.planned_qty_lots
            )) {
            snapshot.fallback_required = true;
            snapshot.reject_reason = FastPathRejectReason::UnsupportedExecutableSide;
            finalize(&snapshot);
            return snapshot;
        }
        leg.vwap = trading_engine::common::math::buy_vwap_linear(
            leg.depth->asks.data(),
            leg.depth->ask_count,
            leg.planned_qty_lots
        );
        if (!leg.vwap.ok) {
            snapshot.fallback_required = true;
            snapshot.reject_reason = FastPathRejectReason::BadDepthView;
            finalize(&snapshot);
            return snapshot;
        }
        if (!trading_engine::common::math::checked_add_i64(
                total_cost_tick,
                leg.vwap.total_cost_tick,
                &total_cost_tick
            )) {
            snapshot.fallback_required = true;
            snapshot.reject_reason = FastPathRejectReason::UnsupportedExecutableSide;
            finalize(&snapshot);
            return snapshot;
        }
    }

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

    snapshot.produced_intent = true;
    snapshot.bundle_id = spec.bundle_id;
    snapshot.intent_status = trading_engine::signal::IntentStatus::PaperOpportunity;
    snapshot.bundle_qty = bundle_qty;
    snapshot.total_cost_tick = total_cost_tick;
    snapshot.unit_edge_tick = edge.unit_edge_tick;
    snapshot.total_edge_tick = edge.total_edge_tick;
    snapshot.edge_bps = edge.edge_bps;

    auto rejection = edge_rejection(spec, edge, bundle_qty, policy);
    if (rejection == trading_engine::risk::RiskDecisionType::Approve) {
        rejection = risk_limit_rejection(
            spec,
            legs,
            total_cost_tick,
            policy,
            ledger
        );
    }

    if (rejection == trading_engine::risk::RiskDecisionType::Approve) {
        snapshot.produced_plan = true;
        snapshot.order_count = spec.leg_count;
        snapshot.risk_reject_reason =
            trading_engine::risk::RiskRejectReason::None;
    } else {
        if (rejection ==
                trading_engine::risk::RiskDecisionType::RejectLowUnitEdge ||
            rejection ==
                trading_engine::risk::RiskDecisionType::RejectLowTotalEdge ||
            rejection ==
                trading_engine::risk::RiskDecisionType::RejectLowEdgeBps ||
            rejection ==
                trading_engine::risk::RiskDecisionType::RejectInsufficientDepth) {
            snapshot.intent_status =
                trading_engine::signal::IntentStatus::RejectedLowEdge;
        }
        snapshot.risk_reject_reason = risk_reject_reason(rejection);
    }

    finalize(&snapshot);
    return snapshot;
}

DifferentialCompareResult compare_decision_snapshots(
    const DecisionPathSnapshot& fast,
    const DecisionPathSnapshot& generic
) {
    DifferentialCompareResult result;
    result.match = true;
    result.fast_opportunity_hash = fast.opportunity_hash;
    result.generic_opportunity_hash = generic.opportunity_hash;
    result.fast_risk_hash = fast.risk_hash;
    result.generic_risk_hash = generic.risk_hash;
    result.fast_plan_hash = fast.plan_hash;
    result.generic_plan_hash = generic.plan_hash;
    result.fast_combined_hash = fast.combined_hash;
    result.generic_combined_hash = generic.combined_hash;
    result.fast_hash = fast.combined_hash;
    result.generic_hash = generic.combined_hash;

    compare_field(
        "opportunity_hash",
        fast.opportunity_hash,
        generic.opportunity_hash,
        &result
    );
    compare_field("risk_hash", fast.risk_hash, generic.risk_hash, &result);
    compare_field("plan_hash", fast.plan_hash, generic.plan_hash, &result);
    compare_field(
        "combined_hash",
        fast.combined_hash,
        generic.combined_hash,
        &result
    );
    compare_field("produced_intent", fast.produced_intent, generic.produced_intent, &result);
    compare_field("produced_plan", fast.produced_plan, generic.produced_plan, &result);
    compare_field("fallback_required", fast.fallback_required, generic.fallback_required, &result);
    compare_field("reject_reason", fast.reject_reason, generic.reject_reason, &result);
    compare_field("intent_status", fast.intent_status, generic.intent_status, &result);
    compare_field(
        "risk_reject_reason",
        fast.risk_reject_reason,
        generic.risk_reject_reason,
        &result
    );
    compare_field("bundle_id", fast.bundle_id, generic.bundle_id, &result);
    compare_field("bundle_qty", fast.bundle_qty, generic.bundle_qty, &result);
    compare_field("total_cost_tick", fast.total_cost_tick, generic.total_cost_tick, &result);
    compare_field("unit_edge_tick", fast.unit_edge_tick, generic.unit_edge_tick, &result);
    compare_field("total_edge_tick", fast.total_edge_tick, generic.total_edge_tick, &result);
    compare_field("edge_bps", fast.edge_bps, generic.edge_bps, &result);
    compare_field("order_count", fast.order_count, generic.order_count, &result);
    if (fast.decision_hash != 0 && generic.decision_hash != 0) {
        compare_field(
            "decision_hash",
            fast.decision_hash,
            generic.decision_hash,
            &result
        );
    }
    if (fast.plan_id != 0 && generic.plan_id != 0) {
        compare_field("plan_id", fast.plan_id, generic.plan_id, &result);
    }
    compare_field("semantic_hash", fast.semantic_hash, generic.semantic_hash, &result);
    return result;
}

}  // namespace trading_engine::decision_fastpath
