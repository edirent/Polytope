#include "engine/risk/guards/PartialFillGuard.h"

#include "oracle/public/CandidateBundle.h"

#include <cmath>
#include <limits>
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

[[nodiscard]] std::int64_t required_with_margin_bps(
    std::int64_t requested_qty_lots,
    std::int64_t margin_bps
) noexcept {
    if (requested_qty_lots <= 0) {
        return 0;
    }
    if (margin_bps < 10'000) {
        margin_bps = 10'000;
    }

    const auto value =
        static_cast<__int128>(requested_qty_lots) *
        static_cast<__int128>(margin_bps);
    const auto required = (value + 9'999) / 10'000;
    if (required > std::numeric_limits<std::int64_t>::max()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(required);
}

[[nodiscard]] std::int64_t depth_margin_bps(
    std::int64_t executable_qty_lots,
    std::int64_t requested_qty_lots
) noexcept {
    if (requested_qty_lots <= 0 || executable_qty_lots <= 0) {
        return 0;
    }
    const auto value =
        static_cast<__int128>(executable_qty_lots) * 10'000 /
        static_cast<__int128>(requested_qty_lots);
    if (value > std::numeric_limits<std::int64_t>::max()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] std::int64_t effective_min_depth_margin_bps(
    const RiskPolicySnapshot& policy
) noexcept {
    constexpr auto kDefaultRatio = 1.20;
    constexpr auto kDefaultBps = 12'000;

    if (policy.min_depth_margin_bps > 0 &&
        (policy.min_depth_margin_bps != kDefaultBps ||
         std::abs(policy.min_depth_margin_ratio - kDefaultRatio) < 0.000001)) {
        return policy.min_depth_margin_bps;
    }

    auto ratio = policy.min_depth_margin_ratio;
    if (!std::isfinite(ratio) || ratio < 1.0) {
        ratio = 1.0;
    }
    const auto bps = std::ceil(ratio * 10'000.0);
    if (bps >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(bps);
}

}  // namespace

PartialFillGuardResult PartialFillGuard::check(
    const signal::OpportunityIntent& intent,
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

    result.depth_margin_checked = true;
    const auto min_depth_margin_bps = effective_min_depth_margin_bps(policy);

    if (cost.leg_count < intent.leg_count) {
        return reject(
            RiskDecisionType::RejectPartialFillRisk,
            "missing revalidated leg depth",
            result
        );
    }

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

        const auto& leg_cost = cost.legs[i];
        if (!leg_cost.asset_id.empty() && leg_cost.asset_id != leg.asset_id) {
            result.rejected_asset_id = leg.asset_id;
            return reject(
                RiskDecisionType::RejectPartialFillRisk,
                "revalidated leg asset mismatch",
                result
            );
        }

        auto requested_qty_lots = leg_cost.requested_qty_lots;
        if (requested_qty_lots <= 0) {
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
        }

        const auto available_depth_lots = leg_cost.executable_qty_lots;
        const auto margin_bps = leg_cost.depth_margin_bps > 0
            ? leg_cost.depth_margin_bps
            : depth_margin_bps(available_depth_lots, requested_qty_lots);

        const auto required_depth_lots = required_with_margin_bps(
            requested_qty_lots,
            min_depth_margin_bps
        );
        if (!leg_cost.enough_depth || margin_bps < min_depth_margin_bps) {
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
