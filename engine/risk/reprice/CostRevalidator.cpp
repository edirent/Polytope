#include "engine/risk/reprice/CostRevalidator.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace trading_engine::risk {

namespace {

[[nodiscard]] std::uint64_t elapsed_ns(
    std::chrono::steady_clock::time_point start
) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start
        ).count()
    );
}

[[nodiscard]] const state::MarketStateSnapshot* find_snapshot(
    const state::MarketStateSnapshot* snapshots,
    std::size_t snapshot_count,
    const std::string& asset_id
) noexcept {
    if (snapshots == nullptr) {
        return nullptr;
    }
    for (std::size_t i = 0; i < snapshot_count; ++i) {
        if (snapshots[i].entity_id == asset_id) {
            return &snapshots[i];
        }
    }
    return nullptr;
}

[[nodiscard]] const state::MarketDepthView* find_depth_view(
    const state::MarketDepthView* depth_views,
    std::size_t depth_view_count,
    std::uint32_t asset_index
) noexcept {
    if (depth_views == nullptr) {
        return nullptr;
    }
    for (std::size_t i = 0; i < depth_view_count; ++i) {
        if (depth_views[i].asset_index == asset_index) {
            return &depth_views[i];
        }
    }
    return nullptr;
}

[[nodiscard]] std::int64_t saturating_mul_i64(
    std::int64_t lhs,
    std::int64_t rhs
) noexcept {
    const auto value =
        static_cast<__int128>(lhs) * static_cast<__int128>(rhs);
    if (value > std::numeric_limits<std::int64_t>::max()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value < std::numeric_limits<std::int64_t>::min()) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
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

[[nodiscard]] bool already_seen_asset(
    const signal::OpportunityIntent& intent,
    std::uint16_t current_index
) noexcept {
    const auto& current = intent.legs[current_index].asset_id;
    for (std::uint16_t i = 0; i < current_index; ++i) {
        if (intent.legs[i].asset_id == current) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool combined_snapshot_hash_matches(
    const signal::OpportunityIntent& intent,
    std::uint64_t snapshot_version_hash
) noexcept {
    if (intent.snapshot_version_hash == 0 || intent.leg_count == 0 ||
        intent.leg_count > signal::kMaxIntentLegs) {
        return false;
    }
    return snapshot_version_hash != 0 &&
           snapshot_version_hash == intent.snapshot_version_hash;
}

[[nodiscard]] bool snapshots_are_fresh(
    const signal::OpportunityIntent& intent,
    const state::MarketStateSnapshot* snapshots,
    std::size_t snapshot_count,
    const RiskPolicySnapshot& policy,
    std::uint64_t now_ns
) noexcept {
    if (now_ns == 0) {
        return false;
    }

    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto& leg = intent.legs[i];
        if (leg.asset_id.empty() || already_seen_asset(intent, i)) {
            continue;
        }

        const auto* snapshot =
            find_snapshot(snapshots, snapshot_count, leg.asset_id);
        if (snapshot == nullptr || snapshot->last_book_update_ns == 0 ||
            now_ns < snapshot->last_book_update_ns) {
            return false;
        }

        const auto age_ns = static_cast<std::int64_t>(
            now_ns - snapshot->last_book_update_ns
        );
        if (policy.max_book_age_ns >= 0 && age_ns > policy.max_book_age_ns) {
            return false;
        }
    }

    return true;
}

[[nodiscard]] bool depth_views_are_fresh(
    const signal::OpportunityIntent& intent,
    const state::MarketDepthView* depth_views,
    std::size_t depth_view_count,
    const RiskPolicySnapshot& policy,
    std::uint64_t now_ns
) noexcept {
    if (now_ns == 0) {
        return false;
    }

    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto& leg = intent.legs[i];
        if (leg.asset_id.empty() || already_seen_asset(intent, i)) {
            continue;
        }

        const auto* depth_view =
            find_depth_view(depth_views, depth_view_count, leg.asset_index);
        if (depth_view == nullptr || depth_view->last_ws_recv_ns == 0 ||
            now_ns < depth_view->last_ws_recv_ns) {
            return false;
        }

        const auto age_ns = static_cast<std::int64_t>(
            now_ns - depth_view->last_ws_recv_ns
        );
        if (policy.max_book_age_ns >= 0 && age_ns > policy.max_book_age_ns) {
            return false;
        }
    }

    return true;
}

[[nodiscard]] bool can_reuse_signal_snapshot(
    const signal::OpportunityIntent& intent,
    const state::MarketStateSnapshot* snapshots,
    std::size_t snapshot_count,
    const RiskPolicySnapshot& policy,
    std::uint64_t now_ns,
    std::uint64_t snapshot_version_hash
) noexcept {
    if (intent.expires_at_ns <= now_ns || intent.bundle_qty <= 0 ||
        intent.estimated_cost_tick < 0 || intent.leg_count == 0 ||
        intent.leg_count > signal::kMaxIntentLegs) {
        return false;
    }

    const auto original_bundle_qty = intent.original_bundle_qty > 0
        ? intent.original_bundle_qty
        : intent.bundle_qty;
    if (intent.bundle_qty > original_bundle_qty) {
        return false;
    }

    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto& leg = intent.legs[i];
        if (leg.side != signal::Side::Buy ||
            leg.asset_id.empty() || leg.quantity_lots <= 0 ||
            !leg.enough_depth) {
            return false;
        }
    }

    return combined_snapshot_hash_matches(intent, snapshot_version_hash) &&
           snapshots_are_fresh(intent, snapshots, snapshot_count, policy, now_ns);
}

[[nodiscard]] bool can_reuse_signal_depth_view(
    const signal::OpportunityIntent& intent,
    const state::MarketDepthView* depth_views,
    std::size_t depth_view_count,
    const RiskPolicySnapshot& policy,
    std::uint64_t now_ns,
    std::uint64_t snapshot_version_hash
) noexcept {
    if (intent.expires_at_ns <= now_ns || intent.bundle_qty <= 0 ||
        intent.estimated_cost_tick < 0 || intent.leg_count == 0 ||
        intent.leg_count > signal::kMaxIntentLegs) {
        return false;
    }

    const auto original_bundle_qty = intent.original_bundle_qty > 0
        ? intent.original_bundle_qty
        : intent.bundle_qty;
    if (intent.bundle_qty > original_bundle_qty) {
        return false;
    }

    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto& leg = intent.legs[i];
        if (leg.side != signal::Side::Buy ||
            leg.asset_id.empty() || leg.quantity_lots <= 0 ||
            !leg.enough_depth) {
            return false;
        }
    }

    return combined_snapshot_hash_matches(intent, snapshot_version_hash) &&
           depth_views_are_fresh(
               intent,
               depth_views,
               depth_view_count,
               policy,
               now_ns
           );
}

[[nodiscard]] CostRevalidationResult reuse_signal_snapshot(
    const signal::OpportunityIntent& intent
) noexcept {
    CostRevalidationResult result;
    result.ok = true;
    result.rejection = RiskDecisionType::Approve;
    result.vwap_mode = RiskVWAPMode::ReuseSignalSnapshot;
    result.risk_total_cost_tick = intent.estimated_cost_tick;
    result.risk_bundle_qty = intent.bundle_qty;
    result.max_leg_slippage_tick = intent.max_leg_slippage_tick;
    result.cost_drift_tick = 0;
    result.leg_count = std::min<std::uint16_t>(
        intent.leg_count,
        kMaxRevalidatedLegCosts
    );
    for (std::uint16_t i = 0; i < result.leg_count; ++i) {
        const auto& source = intent.legs[i];
        auto& target = result.legs[i];
        target.asset_id = source.asset_id;
        target.asset_index = source.asset_index;
        target.requested_qty_lots =
            source.requested_qty_lots > 0
                ? source.requested_qty_lots
                : saturating_mul_i64(source.quantity_lots, intent.bundle_qty);
        target.executable_qty_lots =
            source.executable_qty_lots > 0
                ? source.executable_qty_lots
                : (source.enough_depth ? target.requested_qty_lots : 0);
        target.depth_margin_bps =
            source.depth_margin_bps > 0
                ? source.depth_margin_bps
                : depth_margin_bps(
                      target.executable_qty_lots,
                      target.requested_qty_lots
                  );
        target.enough_depth =
            source.enough_depth &&
            target.executable_qty_lots >= target.requested_qty_lots;
    }
    return result;
}

void accumulate_cost_timing(
    RiskStageTimings* timings,
    std::chrono::steady_clock::time_point total_start
) noexcept {
    if (timings != nullptr) {
        timings->cost_revalidator_ns += elapsed_ns(total_start);
    }
}

}  // namespace

CostRevalidationResult CostRevalidator::revalidate(
    const signal::OpportunityIntent& intent,
    const std::vector<state::MarketStateSnapshot>& snapshots,
    const RiskPolicySnapshot& policy
) const {
    return revalidate(intent, snapshots, policy, 0, nullptr);
}

CostRevalidationResult CostRevalidator::revalidate(
    const signal::OpportunityIntent& intent,
    const std::vector<state::MarketStateSnapshot>& snapshots,
    const RiskPolicySnapshot& policy,
    RiskStageTimings* timings
) const {
    return revalidate(intent, snapshots, policy, 0, timings);
}

CostRevalidationResult CostRevalidator::revalidate(
    const signal::OpportunityIntent& intent,
    const std::vector<state::MarketStateSnapshot>& snapshots,
    const RiskPolicySnapshot& policy,
    std::uint64_t now_ns,
    RiskStageTimings* timings
) const {
    return revalidate(intent, snapshots, policy, now_ns, 0, timings);
}

CostRevalidationResult CostRevalidator::revalidate(
    const signal::OpportunityIntent& intent,
    const std::vector<state::MarketStateSnapshot>& snapshots,
    const RiskPolicySnapshot& policy,
    std::uint64_t now_ns,
    std::uint64_t snapshot_version_hash,
    RiskStageTimings* timings
) const {
    return revalidate(
        intent,
        snapshots.data(),
        snapshots.size(),
        policy,
        now_ns,
        snapshot_version_hash,
        timings
    );
}

CostRevalidationResult CostRevalidator::revalidate(
    const signal::OpportunityIntent& intent,
    const state::MarketStateSnapshot* snapshots,
    std::size_t snapshot_count,
    const RiskPolicySnapshot& policy,
    std::uint64_t now_ns,
    std::uint64_t snapshot_version_hash,
    RiskStageTimings* timings
) const {
    using Clock = std::chrono::steady_clock;
    const auto total_start = Clock::now();
    const auto vwap_start = Clock::now();
    auto result = can_reuse_signal_snapshot(
                      intent,
                      snapshots,
                      snapshot_count,
                      policy,
                      now_ns,
                      snapshot_version_hash
                  )
        ? reuse_signal_snapshot(intent)
        : vwap_.reprice(intent, snapshots, snapshot_count);
    if (timings != nullptr) {
        timings->vwap_revalidator_ns += elapsed_ns(vwap_start);
    }
    if (!result.ok) {
        accumulate_cost_timing(timings, total_start);
        return result;
    }

    result.fee_tick = fee_.estimate_fee_tick(intent);
    result.slippage_buffer_tick =
        slippage_.estimate_slippage_buffer_tick(intent);
    result.latency_buffer_tick = latency_.estimate_latency_buffer_tick(intent);
    result.cost_drift_tick =
        result.risk_total_cost_tick - intent.estimated_cost_tick;

    if (result.cost_drift_tick > policy.max_allowed_cost_drift_tick) {
        result.ok = false;
        result.rejection = RiskDecisionType::RejectCostDrift;
        result.reason = "cost drift exceeds policy";
        accumulate_cost_timing(timings, total_start);
        return result;
    }

    result.ok = true;
    result.rejection = RiskDecisionType::Approve;
    accumulate_cost_timing(timings, total_start);
    return result;
}

CostRevalidationResult CostRevalidator::revalidate(
    const signal::OpportunityIntent& intent,
    const state::MarketDepthView* depth_views,
    std::size_t depth_view_count,
    const RiskPolicySnapshot& policy,
    std::uint64_t now_ns,
    std::uint64_t snapshot_version_hash,
    RiskStageTimings* timings
) const {
    using Clock = std::chrono::steady_clock;
    const auto total_start = Clock::now();
    const auto vwap_start = Clock::now();
    auto result = can_reuse_signal_depth_view(
                      intent,
                      depth_views,
                      depth_view_count,
                      policy,
                      now_ns,
                      snapshot_version_hash
                  )
        ? reuse_signal_snapshot(intent)
        : vwap_.reprice(intent, depth_views, depth_view_count);
    if (timings != nullptr) {
        timings->vwap_revalidator_ns += elapsed_ns(vwap_start);
    }
    if (!result.ok) {
        accumulate_cost_timing(timings, total_start);
        return result;
    }

    result.fee_tick = fee_.estimate_fee_tick(intent);
    result.slippage_buffer_tick =
        slippage_.estimate_slippage_buffer_tick(intent);
    result.latency_buffer_tick = latency_.estimate_latency_buffer_tick(intent);
    result.cost_drift_tick =
        result.risk_total_cost_tick - intent.estimated_cost_tick;

    if (result.cost_drift_tick > policy.max_allowed_cost_drift_tick) {
        result.ok = false;
        result.rejection = RiskDecisionType::RejectCostDrift;
        result.reason = "cost drift exceeds policy";
        accumulate_cost_timing(timings, total_start);
        return result;
    }

    result.ok = true;
    result.rejection = RiskDecisionType::Approve;
    accumulate_cost_timing(timings, total_start);
    return result;
}

}  // namespace trading_engine::risk
