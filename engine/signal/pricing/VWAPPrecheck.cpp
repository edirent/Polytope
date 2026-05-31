#include "engine/signal/pricing/VWAPPrecheck.h"

#include "engine/signal/pricing/PriceVectorBuilder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace trading_engine::signal {

namespace {

[[nodiscard]] CostResult failure(CostFailureReason reason) {
    CostResult result;
    result.failure_reason = reason;
    return result;
}

[[nodiscard]] std::uint64_t elapsed_ns(
    std::chrono::steady_clock::time_point start
) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start
        ).count()
    );
}

[[nodiscard]] std::int64_t level_size_lots(
    const trading_engine::state::PriceLevel& level
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

[[nodiscard]] bool checked_add_cost(
    std::int64_t price_tick,
    std::int64_t quantity_lots,
    std::int64_t* total
) noexcept {
    const auto add =
        static_cast<__int128>(price_tick) *
        static_cast<__int128>(quantity_lots);
    const auto next = static_cast<__int128>(*total) + add;
    if (next > std::numeric_limits<std::int64_t>::max() ||
        next < std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    *total = static_cast<std::int64_t>(next);
    return true;
}

struct LegCapacity {
    FillSimulationLeg leg;
    std::int64_t best_price_tick = 0;
};

[[nodiscard]] const MarketStateSnapshot* snapshot_for_asset(
    const SnapshotBatchReadResult& snapshots,
    const std::string& asset_id
) noexcept {
    for (std::uint16_t i = 0; i < snapshots.snapshot_count; ++i) {
        if (snapshots.snapshots[i].entity_id == asset_id) {
            return &snapshots.snapshots[i];
        }
    }
    return nullptr;
}

[[nodiscard]] const trading_engine::state::MarketDepthView* depth_for_asset(
    const DepthReadResult& depth,
    std::uint32_t asset_index
) noexcept {
    for (std::uint16_t i = 0; i < depth.count; ++i) {
        if (depth.depth_views[i].asset_index == asset_index) {
            return &depth.depth_views[i];
        }
    }
    return nullptr;
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

[[nodiscard]] std::int64_t ceil_div_positive(
    std::int64_t numerator,
    std::int64_t denominator
) noexcept {
    if (denominator <= 0) {
        return 0;
    }
    const auto value =
        (static_cast<__int128>(numerator) + denominator - 1) /
        static_cast<__int128>(denominator);
    if (value > std::numeric_limits<std::int64_t>::max()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] CostFailureReason calculate_buy_capacity(
    const PriceLeg& leg,
    LegCapacity* out
) {
    if (leg.target_qty_lots <= 0) {
        return CostFailureReason::InvalidQuantity;
    }
    if (leg.executable_side != ExecutableBookSide::Asks || !leg.snapshot) {
        return CostFailureReason::InvalidLeg;
    }
    const auto& snapshot = *leg.snapshot;
    if (!snapshot.has_ask || snapshot.ask_count == 0) {
        return CostFailureReason::MissingBookSide;
    }

    out->leg.asset_id = leg.asset_id;
    out->leg.requested_qty_lots = leg.target_qty_lots;

    const auto level_count = std::min<std::uint32_t>(
        snapshot.ask_count,
        trading_engine::state::kMaxSnapshotDepth
    );

    for (std::uint32_t i = 0; i < level_count; ++i) {
        const auto& level = snapshot.asks[i];
        const auto available = level_size_lots(level);
        if (level.price_tick <= 0 || available <= 0) {
            continue;
        }
        if (out->best_price_tick == 0) {
            out->best_price_tick = level.price_tick;
        }
        if (!checked_add_i64(available, &out->leg.executable_qty_lots)) {
            return CostFailureReason::InvalidLeg;
        }
    }

    out->leg.enough_depth =
        out->leg.executable_qty_lots >= out->leg.requested_qty_lots;
    return CostFailureReason::None;
}

[[nodiscard]] CostFailureReason calculate_buy_capacity(
    const std::string& asset_id,
    std::int64_t target_qty_lots,
    ExecutableBookSide executable_side,
    const trading_engine::state::MarketDepthView* depth,
    LegCapacity* out
) {
    if (target_qty_lots <= 0) {
        return CostFailureReason::InvalidQuantity;
    }
    if (executable_side != ExecutableBookSide::Asks || !depth) {
        return CostFailureReason::InvalidLeg;
    }
    if (depth->ask_count == 0) {
        return CostFailureReason::MissingBookSide;
    }

    out->leg.asset_id = asset_id;
    out->leg.requested_qty_lots = target_qty_lots;

    if (depth->prefix.ask_count > 0) {
        out->leg.executable_qty_lots =
            trading_engine::state::ask_depth_from_prefix(depth->prefix);
        out->best_price_tick =
            trading_engine::state::best_ask_tick_from_prefix(
                depth->asks,
                depth->prefix
            );
        out->leg.enough_depth =
            out->leg.executable_qty_lots >= out->leg.requested_qty_lots;
        return CostFailureReason::None;
    }

    const auto level_count = std::min<std::uint16_t>(
        depth->ask_count,
        trading_engine::state::kMaxSnapshotDepth
    );

    for (std::uint16_t i = 0; i < level_count; ++i) {
        const auto& level = depth->asks[i];
        const auto available = level_size_lots(level);
        if (level.price_tick <= 0 || available <= 0) {
            continue;
        }
        if (out->best_price_tick == 0) {
            out->best_price_tick = level.price_tick;
        }
        if (!checked_add_i64(available, &out->leg.executable_qty_lots)) {
            return CostFailureReason::InvalidLeg;
        }
    }

    out->leg.enough_depth =
        out->leg.executable_qty_lots >= out->leg.requested_qty_lots;
    return CostFailureReason::None;
}

[[nodiscard]] CostFailureReason calculate_buy_capacity(
    const std::string& asset_id,
    std::int64_t target_qty_lots,
    ExecutableBookSide executable_side,
    const MarketStateSnapshot* snapshot,
    LegCapacity* out
) {
    if (target_qty_lots <= 0) {
        return CostFailureReason::InvalidQuantity;
    }
    if (executable_side != ExecutableBookSide::Asks || !snapshot) {
        return CostFailureReason::InvalidLeg;
    }
    if (!snapshot->has_ask || snapshot->ask_count == 0) {
        return CostFailureReason::MissingBookSide;
    }

    out->leg.asset_id = asset_id;
    out->leg.requested_qty_lots = target_qty_lots;

    const auto level_count = std::min<std::uint32_t>(
        snapshot->ask_count,
        trading_engine::state::kMaxSnapshotDepth
    );

    for (std::uint32_t i = 0; i < level_count; ++i) {
        const auto& level = snapshot->asks[i];
        const auto available = level_size_lots(level);
        if (level.price_tick <= 0 || available <= 0) {
            continue;
        }
        if (out->best_price_tick == 0) {
            out->best_price_tick = level.price_tick;
        }
        if (!checked_add_i64(available, &out->leg.executable_qty_lots)) {
            return CostFailureReason::InvalidLeg;
        }
    }

    out->leg.enough_depth =
        out->leg.executable_qty_lots >= out->leg.requested_qty_lots;
    return CostFailureReason::None;
}

[[nodiscard]] CostFailureReason price_buy_leg_for_quantity(
    const PriceLeg& leg,
    std::int64_t quantity_lots,
    FillSimulationLeg* out
) {
    if (quantity_lots <= 0) {
        return CostFailureReason::InvalidQuantity;
    }
    if (!leg.snapshot) {
        return CostFailureReason::InvalidLeg;
    }

    const auto& snapshot = *leg.snapshot;
    const auto level_count = std::min<std::uint32_t>(
        snapshot.ask_count,
        trading_engine::state::kMaxSnapshotDepth
    );

    std::int64_t remaining = quantity_lots;
    std::int64_t leg_cost = 0;
    std::int64_t worst_price_tick = 0;
    std::int64_t best_price_tick = 0;
    for (std::uint32_t i = 0; i < level_count && remaining > 0; ++i) {
        const auto& level = snapshot.asks[i];
        const auto available = level_size_lots(level);
        if (level.price_tick <= 0 || available <= 0) {
            continue;
        }

        if (best_price_tick == 0) {
            best_price_tick = level.price_tick;
        }

        const auto take = std::min(remaining, available);
        if (!checked_add_cost(level.price_tick, take, &leg_cost)) {
            return CostFailureReason::InvalidLeg;
        }
        worst_price_tick = std::max(worst_price_tick, level.price_tick);
        remaining -= take;
    }

    if (remaining > 0) {
        return CostFailureReason::InsufficientDepth;
    }

    out->asset_id = leg.asset_id;
    out->total_cost_tick = leg_cost;
    out->vwap_price_tick = leg_cost / quantity_lots;
    out->worst_price_tick = worst_price_tick;
    out->enough_depth = true;
    out->book_age_ns = 0;
    if (best_price_tick > 0) {
        // Slippage is derived by the caller from worst minus best.
    }
    return CostFailureReason::None;
}

[[nodiscard]] CostFailureReason price_buy_leg_for_quantity(
    const std::string& asset_id,
    const trading_engine::state::MarketDepthView* depth,
    std::int64_t quantity_lots,
    FillSimulationLeg* out
) {
    if (quantity_lots <= 0) {
        return CostFailureReason::InvalidQuantity;
    }
    if (!depth) {
        return CostFailureReason::InvalidLeg;
    }

    if (depth->prefix.ask_count > 0) {
        const auto prefix_result =
            trading_engine::state::buy_vwap_from_prefix(*depth, quantity_lots);
        if (!prefix_result.ok) {
            return CostFailureReason::InsufficientDepth;
        }

        out->asset_id = asset_id;
        out->total_cost_tick = prefix_result.total_cost_tick;
        out->vwap_price_tick = prefix_result.vwap_tick;
        out->worst_price_tick = prefix_result.worst_price_tick;
        out->enough_depth = true;
        out->book_age_ns = 0;
        return CostFailureReason::None;
    }

    const auto level_count = std::min<std::uint16_t>(
        depth->ask_count,
        trading_engine::state::kMaxSnapshotDepth
    );

    std::int64_t remaining = quantity_lots;
    std::int64_t leg_cost = 0;
    std::int64_t worst_price_tick = 0;
    for (std::uint16_t i = 0; i < level_count && remaining > 0; ++i) {
        const auto& level = depth->asks[i];
        const auto available = level_size_lots(level);
        if (level.price_tick <= 0 || available <= 0) {
            continue;
        }

        const auto take = std::min(remaining, available);
        if (!checked_add_cost(level.price_tick, take, &leg_cost)) {
            return CostFailureReason::InvalidLeg;
        }
        worst_price_tick = std::max(worst_price_tick, level.price_tick);
        remaining -= take;
    }

    if (remaining > 0) {
        return CostFailureReason::InsufficientDepth;
    }

    out->asset_id = asset_id;
    out->total_cost_tick = leg_cost;
    out->vwap_price_tick = leg_cost / quantity_lots;
    out->worst_price_tick = worst_price_tick;
    out->enough_depth = true;
    out->book_age_ns = 0;
    return CostFailureReason::None;
}

[[nodiscard]] CostFailureReason price_buy_leg_for_quantity(
    const std::string& asset_id,
    const MarketStateSnapshot* snapshot,
    std::int64_t quantity_lots,
    FillSimulationLeg* out
) {
    if (quantity_lots <= 0) {
        return CostFailureReason::InvalidQuantity;
    }
    if (!snapshot) {
        return CostFailureReason::InvalidLeg;
    }

    const auto level_count = std::min<std::uint32_t>(
        snapshot->ask_count,
        trading_engine::state::kMaxSnapshotDepth
    );

    std::int64_t remaining = quantity_lots;
    std::int64_t leg_cost = 0;
    std::int64_t worst_price_tick = 0;
    for (std::uint32_t i = 0; i < level_count && remaining > 0; ++i) {
        const auto& level = snapshot->asks[i];
        const auto available = level_size_lots(level);
        if (level.price_tick <= 0 || available <= 0) {
            continue;
        }

        const auto take = std::min(remaining, available);
        if (!checked_add_cost(level.price_tick, take, &leg_cost)) {
            return CostFailureReason::InvalidLeg;
        }
        worst_price_tick = std::max(worst_price_tick, level.price_tick);
        remaining -= take;
    }

    if (remaining > 0) {
        return CostFailureReason::InsufficientDepth;
    }

    out->asset_id = asset_id;
    out->total_cost_tick = leg_cost;
    out->vwap_price_tick = leg_cost / quantity_lots;
    out->worst_price_tick = worst_price_tick;
    out->enough_depth = true;
    out->book_age_ns = 0;
    return CostFailureReason::None;
}

}  // namespace

CostResult VWAPPrecheck::price_bundle(
    const CandidateBundle& bundle,
    const std::vector<MarketStateSnapshot>& snapshots
) const {
    const auto vector_start = std::chrono::steady_clock::now();
    const auto vector_result = build_price_vector(bundle, snapshots);
    const auto vector_ns = elapsed_ns(vector_start);
    if (!vector_result.ok) {
        auto result = failure(vector_result.failure_reason);
        result.price_vector_builder_ns = vector_ns;
        return result;
    }

    const auto vwap_start = std::chrono::steady_clock::now();
    auto finish = [&](CostResult result) {
        result.price_vector_builder_ns = vector_ns;
        result.vwap_precheck_ns = elapsed_ns(vwap_start);
        return result;
    };

    CostResult result;
    std::vector<LegCapacity> capacities;
    capacities.reserve(bundle.leg_count);
    result.legs.reserve(bundle.leg_count);

    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        const auto& leg = vector_result.legs[i];

        LegCapacity capacity;
        const auto reason = calculate_buy_capacity(leg, &capacity);
        if (reason != CostFailureReason::None) {
            result.failure_reason = reason;
            result.legs.push_back(capacity.leg);
            return finish(std::move(result));
        }
        result.legs.push_back(capacity.leg);
        capacities.push_back(capacity);
    }

    result.bundle_qty = std::numeric_limits<std::int64_t>::max();
    for (const auto& leg : result.legs) {
        if (!leg.enough_depth || leg.requested_qty_lots <= 0) {
            result.failure_reason = CostFailureReason::InsufficientDepth;
            result.executable = false;
            return finish(std::move(result));
        }
        result.bundle_qty = std::min(
            result.bundle_qty,
            leg.executable_qty_lots / leg.requested_qty_lots
        );
    }

    if (result.bundle_qty <= 0 ||
        result.bundle_qty == std::numeric_limits<std::int64_t>::max()) {
        result.failure_reason = CostFailureReason::InsufficientDepth;
        result.executable = false;
        return finish(std::move(result));
    }

    std::int64_t total_bundle_lots = 0;
    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        const auto& price_leg = vector_result.legs[i];

        std::int64_t planned_qty_lots = 0;
        if (!checked_mul_i64(
                price_leg.target_qty_lots,
                result.bundle_qty,
                &planned_qty_lots
            )) {
            return finish(failure(CostFailureReason::InvalidQuantity));
        }

        auto priced = result.legs[i];
        const auto reason = price_buy_leg_for_quantity(
            price_leg,
            planned_qty_lots,
            &priced
        );
        if (reason != CostFailureReason::None) {
            result.failure_reason = reason;
            result.executable = false;
            result.legs[i] = priced;
            return finish(std::move(result));
        }

        result.max_leg_slippage_tick = std::max(
            result.max_leg_slippage_tick,
            priced.worst_price_tick - capacities[i].best_price_tick
        );
        if (!checked_add_i64(priced.total_cost_tick, &result.total_cost_tick)) {
            return finish(failure(CostFailureReason::InvalidLeg));
        }
        if (!checked_add_i64(planned_qty_lots, &total_bundle_lots)) {
            return finish(failure(CostFailureReason::InvalidQuantity));
        }
        result.legs[i] = priced;
    }

    if (total_bundle_lots <= 0) {
        return finish(failure(CostFailureReason::InvalidQuantity));
    }

    result.avg_cost_tick = ceil_div_positive(
        result.total_cost_tick,
        result.bundle_qty
    );
    result.executable = true;
    result.failure_reason = CostFailureReason::None;
    return finish(std::move(result));
}

CostResult VWAPPrecheck::price_runtime_plan(
    const BundleRuntimePlan& plan,
    const SnapshotBatchReadResult& snapshots
) const {
    const auto vwap_start = std::chrono::steady_clock::now();
    auto finish = [&](CostResult result) {
        result.vwap_precheck_ns = elapsed_ns(vwap_start);
        return result;
    };

    if (!plan.bundle || plan.leg_count == 0 ||
        plan.leg_count > kMaxIntentLegs || !snapshots.ok) {
        return finish(failure(CostFailureReason::InvalidLeg));
    }

    CostResult result;
    std::array<LegCapacity, kMaxIntentLegs> capacities{};
    std::array<const MarketStateSnapshot*, kMaxIntentLegs> leg_snapshots{};

    for (std::uint16_t i = 0; i < plan.leg_count; ++i) {
        if (!plan.asset_ids[i]) {
            return finish(failure(CostFailureReason::InvalidLeg));
        }

        const auto* snapshot = snapshot_for_asset(
            snapshots,
            *plan.asset_ids[i]
        );
        if (!snapshot) {
            return finish(failure(CostFailureReason::MissingSnapshot));
        }
        leg_snapshots[i] = snapshot;

        const auto reason = calculate_buy_capacity(
            *plan.asset_ids[i],
            plan.ratio_qty_lots[i],
            plan.executable_sides[i],
            snapshot,
            &capacities[i]
        );
        result.fixed_legs[i] = capacities[i].leg;
        result.fixed_leg_count = static_cast<std::uint16_t>(i + 1U);
        if (reason != CostFailureReason::None) {
            result.failure_reason = reason;
            return finish(std::move(result));
        }
    }

    result.bundle_qty = std::numeric_limits<std::int64_t>::max();
    for (std::uint16_t i = 0; i < plan.leg_count; ++i) {
        const auto& leg = result.fixed_legs[i];
        if (!leg.enough_depth || leg.requested_qty_lots <= 0) {
            result.failure_reason = CostFailureReason::InsufficientDepth;
            return finish(std::move(result));
        }
        result.bundle_qty = std::min(
            result.bundle_qty,
            leg.executable_qty_lots / leg.requested_qty_lots
        );
    }

    if (result.bundle_qty <= 0 ||
        result.bundle_qty == std::numeric_limits<std::int64_t>::max()) {
        result.failure_reason = CostFailureReason::InsufficientDepth;
        return finish(std::move(result));
    }

    std::int64_t total_bundle_lots = 0;
    for (std::uint16_t i = 0; i < plan.leg_count; ++i) {
        std::int64_t planned_qty_lots = 0;
        if (!checked_mul_i64(
                plan.ratio_qty_lots[i],
                result.bundle_qty,
                &planned_qty_lots
            )) {
            return finish(failure(CostFailureReason::InvalidQuantity));
        }

        const auto* snapshot = leg_snapshots[i];
        auto priced = result.fixed_legs[i];
        const auto reason = price_buy_leg_for_quantity(
            *plan.asset_ids[i],
            snapshot,
            planned_qty_lots,
            &priced
        );
        if (reason != CostFailureReason::None) {
            result.failure_reason = reason;
            result.fixed_legs[i] = priced;
            return finish(std::move(result));
        }

        result.max_leg_slippage_tick = std::max(
            result.max_leg_slippage_tick,
            priced.worst_price_tick - capacities[i].best_price_tick
        );
        if (!checked_add_i64(priced.total_cost_tick, &result.total_cost_tick)) {
            return finish(failure(CostFailureReason::InvalidLeg));
        }
        if (!checked_add_i64(planned_qty_lots, &total_bundle_lots)) {
            return finish(failure(CostFailureReason::InvalidQuantity));
        }
        result.fixed_legs[i] = priced;
    }

    if (total_bundle_lots <= 0) {
        return finish(failure(CostFailureReason::InvalidQuantity));
    }

    result.avg_cost_tick = ceil_div_positive(
        result.total_cost_tick,
        result.bundle_qty
    );
    result.executable = true;
    result.failure_reason = CostFailureReason::None;
    return finish(std::move(result));
}

CostResult VWAPPrecheck::price_runtime_plan(
    const BundleRuntimePlan& plan,
    const DepthReadResult& depth
) const {
    const auto vwap_start = std::chrono::steady_clock::now();
    auto finish = [&](CostResult result) {
        result.vwap_precheck_ns = elapsed_ns(vwap_start);
        return result;
    };

    if (!plan.bundle || plan.leg_count == 0 ||
        plan.leg_count > kMaxIntentLegs || !depth.ok) {
        return finish(failure(CostFailureReason::InvalidLeg));
    }

    CostResult result;
    std::array<LegCapacity, kMaxIntentLegs> capacities{};
    std::array<const trading_engine::state::MarketDepthView*, kMaxIntentLegs>
        leg_depths{};

    for (std::uint16_t i = 0; i < plan.leg_count; ++i) {
        if (!plan.asset_ids[i]) {
            return finish(failure(CostFailureReason::InvalidLeg));
        }

        const auto* depth_view = depth_for_asset(depth, plan.asset_indices[i]);
        if (!depth_view) {
            return finish(failure(CostFailureReason::MissingSnapshot));
        }
        leg_depths[i] = depth_view;

        const auto reason = calculate_buy_capacity(
            *plan.asset_ids[i],
            plan.ratio_qty_lots[i],
            plan.executable_sides[i],
            depth_view,
            &capacities[i]
        );
        result.fixed_legs[i] = capacities[i].leg;
        result.fixed_leg_count = static_cast<std::uint16_t>(i + 1U);
        if (reason != CostFailureReason::None) {
            result.failure_reason = reason;
            return finish(std::move(result));
        }
    }

    result.bundle_qty = std::numeric_limits<std::int64_t>::max();
    for (std::uint16_t i = 0; i < plan.leg_count; ++i) {
        const auto& leg = result.fixed_legs[i];
        if (!leg.enough_depth || leg.requested_qty_lots <= 0) {
            result.failure_reason = CostFailureReason::InsufficientDepth;
            return finish(std::move(result));
        }
        result.bundle_qty = std::min(
            result.bundle_qty,
            leg.executable_qty_lots / leg.requested_qty_lots
        );
    }

    if (result.bundle_qty <= 0 ||
        result.bundle_qty == std::numeric_limits<std::int64_t>::max()) {
        result.failure_reason = CostFailureReason::InsufficientDepth;
        return finish(std::move(result));
    }

    std::int64_t total_bundle_lots = 0;
    for (std::uint16_t i = 0; i < plan.leg_count; ++i) {
        std::int64_t planned_qty_lots = 0;
        if (!checked_mul_i64(
                plan.ratio_qty_lots[i],
                result.bundle_qty,
                &planned_qty_lots
            )) {
            return finish(failure(CostFailureReason::InvalidQuantity));
        }

        const auto* depth_view = leg_depths[i];
        auto priced = result.fixed_legs[i];
        const auto reason = price_buy_leg_for_quantity(
            *plan.asset_ids[i],
            depth_view,
            planned_qty_lots,
            &priced
        );
        if (reason != CostFailureReason::None) {
            result.failure_reason = reason;
            result.fixed_legs[i] = priced;
            return finish(std::move(result));
        }

        result.max_leg_slippage_tick = std::max(
            result.max_leg_slippage_tick,
            priced.worst_price_tick - capacities[i].best_price_tick
        );
        if (!checked_add_i64(priced.total_cost_tick, &result.total_cost_tick)) {
            return finish(failure(CostFailureReason::InvalidLeg));
        }
        if (!checked_add_i64(planned_qty_lots, &total_bundle_lots)) {
            return finish(failure(CostFailureReason::InvalidQuantity));
        }
        result.fixed_legs[i] = priced;
    }

    if (total_bundle_lots <= 0) {
        return finish(failure(CostFailureReason::InvalidQuantity));
    }

    result.avg_cost_tick = ceil_div_positive(
        result.total_cost_tick,
        result.bundle_qty
    );
    result.executable = true;
    result.failure_reason = CostFailureReason::None;
    return finish(std::move(result));
}

}  // namespace trading_engine::signal
