#include "engine/risk/guards/PartialFillGuard.h"

#include "oracle/public/CandidateBundle.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace trading_engine::risk {

namespace {

[[nodiscard]] PartialFillGuardResult reject(
    RiskDecisionType type,
    std::string reason,
    PartialFillGuardResult result
) {
    result.pass = false;
    result.rejection = type;
    result.reason = std::move(reason);
    return result;
}

[[nodiscard]] std::int64_t level_size_lots(
    const state::PriceLevel& level
) noexcept {
    if (!std::isfinite(level.size) || level.size <= 0.0) {
        return 0;
    }
    if (level.size >=
        static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(std::floor(level.size));
}

[[nodiscard]] bool checked_mul_i64(
    std::int64_t lhs,
    std::int64_t rhs,
    std::int64_t* out
) noexcept {
    const auto value =
        static_cast<__int128>(lhs) * static_cast<__int128>(rhs);
    if (value > std::numeric_limits<std::int64_t>::max() ||
        value < std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    *out = static_cast<std::int64_t>(value);
    return true;
}

[[nodiscard]] bool checked_add_i64(
    std::int64_t value,
    std::int64_t* total
) noexcept {
    const auto next =
        static_cast<__int128>(*total) + static_cast<__int128>(value);
    if (next > std::numeric_limits<std::int64_t>::max() ||
        next < std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    *total = static_cast<std::int64_t>(next);
    return true;
}

[[nodiscard]] std::int64_t required_with_margin(
    std::int64_t requested_qty_lots,
    double margin
) noexcept {
    if (requested_qty_lots <= 0) {
        return 0;
    }
    if (!std::isfinite(margin) || margin < 1.0) {
        margin = 1.0;
    }

    const auto required =
        std::ceil(static_cast<long double>(requested_qty_lots) * margin);
    if (required >=
        static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(required);
}

[[nodiscard]] std::int64_t available_ask_depth_lots(
    const state::MarketStateSnapshot& snapshot,
    bool* overflow
) noexcept {
    *overflow = false;
    if (!snapshot.has_ask || snapshot.ask_count == 0) {
        return 0;
    }

    const auto level_count = std::min<std::uint32_t>(
        snapshot.ask_count,
        state::kMaxSnapshotDepth
    );

    std::int64_t out = 0;
    for (std::uint32_t i = 0; i < level_count; ++i) {
        if (!checked_add_i64(level_size_lots(snapshot.asks[i]), &out)) {
            *overflow = true;
            return std::numeric_limits<std::int64_t>::max();
        }
    }
    return out;
}

}  // namespace

PartialFillGuardResult PartialFillGuard::check(
    const signal::OpportunityIntent& intent,
    const std::vector<state::MarketStateSnapshot>& snapshots,
    const CostRevalidationResult& cost,
    const RiskPolicySnapshot& policy
) const {
    PartialFillGuardResult result;
    result.max_unhedged_loss_tick = policy.max_unhedged_loss_tick;

    if (!cost.ok || cost.risk_bundle_qty <= 0) {
        return reject(
            RiskDecisionType::RejectInternalError,
            "invalid cost revalidation result",
            result
        );
    }
    if (intent.leg_count == 0 || intent.leg_count > signal::kMaxIntentLegs) {
        return reject(
            RiskDecisionType::RejectInternalError,
            "invalid intent legs",
            result
        );
    }

    // A single BUY leg has no cross-leg hedge breakage in risk v0.
    if (intent.leg_count == 1) {
        result.pass = true;
        result.rejection = RiskDecisionType::Approve;
        result.depth_margin_checked = false;
        return result;
    }

    std::unordered_map<std::string, const state::MarketStateSnapshot*> by_asset;
    by_asset.reserve(snapshots.size());
    for (const auto& snapshot : snapshots) {
        by_asset.emplace(snapshot.entity_id, &snapshot);
    }

    result.depth_margin_checked = true;

    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto& leg = intent.legs[i];
        if (leg.side != trading_engine::oracle::Side::Buy ||
            leg.asset_id.empty() || leg.quantity_lots <= 0) {
            return reject(
                RiskDecisionType::RejectPartialFillRisk,
                "unsupported or invalid leg for partial fill guard",
                result
            );
        }

        const auto it = by_asset.find(leg.asset_id);
        if (it == by_asset.end() || it->second == nullptr) {
            result.rejected_asset_id = leg.asset_id;
            return reject(
                RiskDecisionType::RejectPartialFillRisk,
                "missing snapshot for partial fill depth check",
                result
            );
        }

        std::int64_t requested_qty_lots = 0;
        if (!checked_mul_i64(
                leg.quantity_lots,
                cost.risk_bundle_qty,
                &requested_qty_lots
            )) {
            result.rejected_asset_id = leg.asset_id;
            return reject(
                RiskDecisionType::RejectPartialFillRisk,
                "requested quantity overflow",
                result
            );
        }

        bool overflow = false;
        const auto available_depth_lots =
            available_ask_depth_lots(*it->second, &overflow);
        if (overflow) {
            result.rejected_asset_id = leg.asset_id;
            return reject(
                RiskDecisionType::RejectPartialFillRisk,
                "available depth overflow",
                result
            );
        }

        const auto required_depth_lots = required_with_margin(
            requested_qty_lots,
            policy.min_depth_margin_ratio
        );
        if (available_depth_lots < required_depth_lots) {
            result.rejected_asset_id = leg.asset_id;
            result.requested_qty_lots = requested_qty_lots;
            result.available_depth_lots = available_depth_lots;
            result.required_depth_with_margin_lots = required_depth_lots;
            return reject(
                RiskDecisionType::RejectPartialFillRisk,
                "depth margin below policy",
                result
            );
        }
    }

    result.pass = true;
    result.rejection = RiskDecisionType::Approve;
    return result;
}

}  // namespace trading_engine::risk
