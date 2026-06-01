#include "engine/risk/reprice/VWAPRevalidator.h"

#include "engine/common/math/FixedPointMath.h"
#include "engine/common/math/VwapMath.h"
#include "oracle/public/CandidateBundle.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace trading_engine::risk {

namespace {

[[nodiscard]] CostRevalidationResult reject(
    RiskDecisionType type,
    std::string reason
) {
    CostRevalidationResult result;
    result.ok = false;
    result.rejection = type;
    result.reason = std::move(reason);
    return result;
}

[[nodiscard]] bool checked_mul_i64(
    std::int64_t lhs,
    std::int64_t rhs,
    std::int64_t* out
) noexcept {
    return trading_engine::common::math::checked_mul_i64(lhs, rhs, out);
}

[[nodiscard]] bool checked_add_i64(
    std::int64_t value,
    std::int64_t* total
) noexcept {
    return trading_engine::common::math::checked_add_i64(*total, value, total);
}

[[nodiscard]] std::int64_t depth_margin_bps(
    std::int64_t executable_qty_lots,
    std::int64_t requested_qty_lots
) noexcept {
    if (requested_qty_lots <= 0 || executable_qty_lots <= 0) {
        return 0;
    }
    return trading_engine::common::math::ratio_bps(
        executable_qty_lots,
        requested_qty_lots
    );
}

struct LegReprice {
    std::int64_t executable_lots = 0;
    std::int64_t cost_tick = 0;
};

[[nodiscard]] bool is_buy(
    const signal::IntentLeg& leg
) noexcept {
    return leg.side == trading_engine::oracle::Side::Buy;
}

[[nodiscard]] CostRevalidationResult reprice_buy_leg(
    const signal::IntentLeg& leg,
    const state::MarketStateSnapshot& snapshot,
    std::int64_t planned_qty_lots,
    LegReprice* out
) {
    if (planned_qty_lots <= 0) {
        return reject(
            RiskDecisionType::RejectInsufficientDepth,
            "invalid planned quantity"
        );
    }
    if (!snapshot.has_ask || snapshot.ask_count == 0) {
        return reject(
            RiskDecisionType::RejectInsufficientDepth,
            "missing ask side"
        );
    }

    const auto level_count = std::min<std::uint32_t>(
        snapshot.ask_count,
        state::kMaxSnapshotDepth
    );

    const auto priced = trading_engine::common::math::buy_vwap_linear(
        snapshot.asks.data(),
        static_cast<std::uint16_t>(level_count),
        planned_qty_lots
    );
    out->executable_lots = priced.executable_qty_lots;
    if (out->executable_lots <= 0) {
        return reject(
            RiskDecisionType::RejectInsufficientDepth,
            "no executable depth"
        );
    }
    if (!priced.ok) {
        return reject(
            RiskDecisionType::RejectReducedBundleQty,
            "latest depth cannot fill original bundle quantity"
        );
    }
    out->cost_tick = priced.total_cost_tick;

    CostRevalidationResult ok;
    ok.ok = true;
    ok.rejection = RiskDecisionType::Approve;
    return ok;
}

[[nodiscard]] CostRevalidationResult reprice_buy_leg(
    const signal::IntentLeg& leg,
    const state::MarketDepthView& depth_view,
    std::int64_t planned_qty_lots,
    LegReprice* out
) {
    if (planned_qty_lots <= 0) {
        return reject(
            RiskDecisionType::RejectInsufficientDepth,
            "invalid planned quantity"
        );
    }
    if (depth_view.ask_count == 0) {
        return reject(
            RiskDecisionType::RejectInsufficientDepth,
            "missing ask side"
        );
    }

    if (depth_view.prefix.ask_count > 0) {
        out->executable_lots =
            state::ask_depth_from_prefix(depth_view.prefix);
        if (out->executable_lots <= 0) {
            return reject(
                RiskDecisionType::RejectInsufficientDepth,
                "no executable depth"
            );
        }
        if (out->executable_lots < planned_qty_lots) {
            return reject(
                RiskDecisionType::RejectReducedBundleQty,
                "latest depth cannot fill original bundle quantity"
            );
        }

        const auto priced =
            trading_engine::common::math::buy_vwap_prefix(
                depth_view.prefix,
                depth_view.asks.data(),
                depth_view.ask_count,
                planned_qty_lots
            );
        if (!priced.ok) {
            return reject(
                RiskDecisionType::RejectInsufficientDepth,
                "prefix vwap failed"
            );
        }
        out->cost_tick = priced.total_cost_tick;

        CostRevalidationResult ok;
        ok.ok = true;
        ok.rejection = RiskDecisionType::Approve;
        return ok;
    }

    const auto level_count = std::min<std::uint16_t>(
        depth_view.ask_count,
        state::kMaxSnapshotDepth
    );
    const auto priced = trading_engine::common::math::buy_vwap_linear(
        depth_view.asks.data(),
        level_count,
        planned_qty_lots
    );
    out->executable_lots = priced.executable_qty_lots;
    if (out->executable_lots <= 0) {
        return reject(
            RiskDecisionType::RejectInsufficientDepth,
            "no executable depth"
        );
    }
    if (!priced.ok) {
        return reject(
            RiskDecisionType::RejectReducedBundleQty,
            "latest depth cannot fill original bundle quantity"
        );
    }
    out->cost_tick = priced.total_cost_tick;

    CostRevalidationResult ok;
    ok.ok = true;
    ok.rejection = RiskDecisionType::Approve;
    return ok;
}

}  // namespace

CostRevalidationResult VWAPRevalidator::reprice(
    const signal::OpportunityIntent& intent,
    const std::vector<state::MarketStateSnapshot>& snapshots
) const {
    return reprice(intent, snapshots.data(), snapshots.size());
}

CostRevalidationResult VWAPRevalidator::reprice(
    const signal::OpportunityIntent& intent,
    const state::MarketStateSnapshot* snapshots,
    std::size_t snapshot_count
) const {
    if (intent.bundle_qty <= 0 || intent.leg_count == 0 ||
        intent.leg_count > signal::kMaxIntentLegs) {
        return reject(
            RiskDecisionType::RejectInsufficientDepth,
            "invalid intent quantity or legs"
        );
    }

    CostRevalidationResult result;
    result.ok = true;
    result.rejection = RiskDecisionType::Approve;
    result.risk_bundle_qty = std::numeric_limits<std::int64_t>::max();
    result.leg_count = std::min<std::uint16_t>(
        intent.leg_count,
        kMaxRevalidatedLegCosts
    );

    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto& leg = intent.legs[i];
        if (!is_buy(leg)) {
            return reject(
                RiskDecisionType::RejectInsufficientDepth,
                "SELL leg repricing is unsupported in risk v0"
            );
        }
        if (leg.asset_id.empty() || leg.quantity_lots <= 0) {
            return reject(
                RiskDecisionType::RejectInsufficientDepth,
                "invalid intent leg"
            );
        }

        const state::MarketStateSnapshot* snapshot = nullptr;
        for (std::size_t snapshot_index = 0; snapshot_index < snapshot_count;
             ++snapshot_index) {
            if (snapshots[snapshot_index].entity_id == leg.asset_id) {
                snapshot = &snapshots[snapshot_index];
                break;
            }
        }
        if (snapshot == nullptr) {
            return reject(
                RiskDecisionType::RejectInsufficientDepth,
                "missing latest snapshot"
            );
        }

        std::int64_t planned_qty_lots = 0;
        if (!checked_mul_i64(
                leg.quantity_lots,
                intent.bundle_qty,
                &planned_qty_lots
            )) {
            return reject(
                RiskDecisionType::RejectInsufficientDepth,
                "planned quantity overflow"
            );
        }

        LegReprice leg_reprice;
        auto leg_result = reprice_buy_leg(
            leg,
            *snapshot,
            planned_qty_lots,
            &leg_reprice
        );
        auto& leg_cost = result.legs[i];
        leg_cost.asset_id = leg.asset_id;
        leg_cost.asset_index = leg.asset_index;
        leg_cost.requested_qty_lots = planned_qty_lots;
        leg_cost.executable_qty_lots = leg_reprice.executable_lots;
        leg_cost.depth_margin_bps = depth_margin_bps(
            leg_reprice.executable_lots,
            planned_qty_lots
        );
        leg_cost.enough_depth = leg_reprice.executable_lots >= planned_qty_lots;
        if (!leg_result.ok) {
            leg_result.leg_count = result.leg_count;
            leg_result.legs = result.legs;
            leg_result.risk_bundle_qty =
                leg.quantity_lots > 0
                    ? leg_reprice.executable_lots / leg.quantity_lots
                    : 0;
            return leg_result;
        }

        result.risk_bundle_qty = std::min(
            result.risk_bundle_qty,
            leg_reprice.executable_lots / leg.quantity_lots
        );
        if (!checked_add_i64(leg_reprice.cost_tick, &result.risk_total_cost_tick)) {
            return reject(
                RiskDecisionType::RejectInsufficientDepth,
                "total cost overflow"
            );
        }
    }

    if (result.risk_bundle_qty == std::numeric_limits<std::int64_t>::max()) {
        return reject(
            RiskDecisionType::RejectInsufficientDepth,
            "no repriced legs"
        );
    }
    if (result.risk_bundle_qty < intent.bundle_qty) {
        result.ok = false;
        result.rejection = RiskDecisionType::RejectReducedBundleQty;
        result.reason = "latest bundle quantity below signal bundle quantity";
        return result;
    }

    return result;
}

CostRevalidationResult VWAPRevalidator::reprice(
    const signal::OpportunityIntent& intent,
    const state::MarketDepthView* depth_views,
    std::size_t depth_view_count
) const {
    if (intent.bundle_qty <= 0 || intent.leg_count == 0 ||
        intent.leg_count > signal::kMaxIntentLegs) {
        return reject(
            RiskDecisionType::RejectInsufficientDepth,
            "invalid intent quantity or legs"
        );
    }

    CostRevalidationResult result;
    result.ok = true;
    result.rejection = RiskDecisionType::Approve;
    result.risk_bundle_qty = std::numeric_limits<std::int64_t>::max();
    result.leg_count = std::min<std::uint16_t>(
        intent.leg_count,
        kMaxRevalidatedLegCosts
    );

    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto& leg = intent.legs[i];
        if (!is_buy(leg)) {
            return reject(
                RiskDecisionType::RejectInsufficientDepth,
                "SELL leg repricing is unsupported in risk v0"
            );
        }
        if (leg.asset_id.empty() || leg.quantity_lots <= 0) {
            return reject(
                RiskDecisionType::RejectInsufficientDepth,
                "invalid intent leg"
            );
        }

        const state::MarketDepthView* depth_view = nullptr;
        for (std::size_t view_index = 0; view_index < depth_view_count;
             ++view_index) {
            if (depth_views[view_index].asset_index == leg.asset_index) {
                depth_view = &depth_views[view_index];
                break;
            }
        }
        if (depth_view == nullptr) {
            return reject(
                RiskDecisionType::RejectInsufficientDepth,
                "missing latest depth view"
            );
        }

        std::int64_t planned_qty_lots = 0;
        if (!checked_mul_i64(
                leg.quantity_lots,
                intent.bundle_qty,
                &planned_qty_lots
            )) {
            return reject(
                RiskDecisionType::RejectInsufficientDepth,
                "planned quantity overflow"
            );
        }

        LegReprice leg_reprice;
        auto leg_result = reprice_buy_leg(
            leg,
            *depth_view,
            planned_qty_lots,
            &leg_reprice
        );
        auto& leg_cost = result.legs[i];
        leg_cost.asset_id = leg.asset_id;
        leg_cost.asset_index = leg.asset_index;
        leg_cost.requested_qty_lots = planned_qty_lots;
        leg_cost.executable_qty_lots = leg_reprice.executable_lots;
        leg_cost.depth_margin_bps = depth_margin_bps(
            leg_reprice.executable_lots,
            planned_qty_lots
        );
        leg_cost.enough_depth = leg_reprice.executable_lots >= planned_qty_lots;
        if (!leg_result.ok) {
            leg_result.leg_count = result.leg_count;
            leg_result.legs = result.legs;
            leg_result.risk_bundle_qty =
                leg.quantity_lots > 0
                    ? leg_reprice.executable_lots / leg.quantity_lots
                    : 0;
            return leg_result;
        }

        result.risk_bundle_qty = std::min(
            result.risk_bundle_qty,
            leg_reprice.executable_lots / leg.quantity_lots
        );
        if (!checked_add_i64(leg_reprice.cost_tick, &result.risk_total_cost_tick)) {
            return reject(
                RiskDecisionType::RejectInsufficientDepth,
                "total cost overflow"
            );
        }
    }

    if (result.risk_bundle_qty == std::numeric_limits<std::int64_t>::max()) {
        return reject(
            RiskDecisionType::RejectInsufficientDepth,
            "no repriced legs"
        );
    }
    if (result.risk_bundle_qty < intent.bundle_qty) {
        result.ok = false;
        result.rejection = RiskDecisionType::RejectReducedBundleQty;
        result.reason = "latest bundle quantity below signal bundle quantity";
        return result;
    }

    return result;
}

}  // namespace trading_engine::risk
