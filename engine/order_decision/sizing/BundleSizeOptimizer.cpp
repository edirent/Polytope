#include "engine/order_decision/sizing/BundleSizeOptimizer.h"

#include "engine/common/math/FixedPointMath.h"
#include "engine/common/math/RiskMath.h"
#include "engine/order_decision/core/OrderDecisionCache.h"
#include "engine/order_decision/math/DiscreteOptimizer.h"
#include "engine/order_decision/math/PrefixVwap.h"
#include "engine/order_decision/sizing/CostCurveBuilder.h"
#include "engine/order_decision/sizing/SizeCandidateEnumerator.h"

#include <algorithm>
#include <array>
#include <chrono>

namespace trading_engine::order_decision {

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsed_ns(
    Clock::time_point start
) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start
        ).count()
    );
}

[[nodiscard]] BundleSizeResult reject(OrderDecisionType reason) {
    BundleSizeResult result;
    result.reject_reason = reason;
    return result;
}

[[nodiscard]] BundleSizeResult reject(
    OrderDecisionType reason,
    const OrderDecisionStageTimings& timings,
    const OrderDecisionEvalStats& eval_stats = {}
) {
    auto result = reject(reason);
    result.timings = timings;
    result.eval_stats = eval_stats;
    return result;
}

[[nodiscard]] std::int64_t ratio_for_leg(
    const oracle::BundleLeg& bundle_leg,
    const signal::IntentLeg* intent_leg
) noexcept {
    if (bundle_leg.quantity_lots > 0) {
        return bundle_leg.quantity_lots;
    }
    if (intent_leg != nullptr && intent_leg->quantity_lots > 0) {
        return intent_leg->quantity_lots;
    }
    return 1;
}

[[nodiscard]] const state::MarketDepthView* find_depth_view(
    std::span<const state::MarketDepthView> depth_views,
    std::uint32_t asset_index,
    std::uint16_t fallback_index
) noexcept {
    for (const auto& view : depth_views) {
        if (view.asset_index == asset_index) {
            return &view;
        }
    }
    if (fallback_index < depth_views.size()) {
        return &depth_views[fallback_index];
    }
    return nullptr;
}

[[nodiscard]] bool depth_flags_usable(
    const state::MarketDepthView& depth
) noexcept {
    return depth.usable_for_depth && !depth.recovering && !depth.crossed &&
           !depth.closed && !depth.resolved;
}

[[nodiscard]] bool checked_add_to(
    std::int64_t value,
    std::int64_t* total
) noexcept {
    return common::math::checked_add_i64(*total, value, total);
}

[[nodiscard]] bool checked_mul(
    std::int64_t lhs,
    std::int64_t rhs,
    std::int64_t* out
) noexcept {
    return common::math::checked_mul_i64(lhs, rhs, out);
}

[[nodiscard]] std::int64_t ceil_ratio_bps(
    std::int64_t value,
    std::int64_t bps
) noexcept {
    if (value <= 0 || bps <= 10'000) {
        return value;
    }
    std::int64_t scaled = 0;
    if (!checked_mul(value, bps, &scaled)) {
        return value;
    }
    return common::math::ceil_div_positive(scaled, 10'000);
}

[[nodiscard]] bool has_ask_prefix(
    const state::MarketDepthView* depth
) noexcept {
    return depth != nullptr && depth->prefix.ask_count > 0 &&
           depth->prefix.ask_count <= depth->ask_count &&
           depth->prefix.ask_cum_qty[depth->prefix.ask_count - 1U] > 0;
}

[[nodiscard]] CostForQuantityResult cost_for_quantity_recorded(
    const CostCurve& curve,
    const state::MarketDepthView* depth,
    std::int64_t qty_lots,
    bool use_prefix_vwap,
    bool debug_compare_prefix_vs_linear,
    OrderDecisionEvalStats* eval_stats
) noexcept {
    if (eval_stats != nullptr) {
        ++eval_stats->vwap_cost_calls;
    }

    if (use_prefix_vwap && has_ask_prefix(depth)) {
        const auto prefix =
            trading_engine::order_decision::buy_vwap_from_prefix(
                *depth,
                qty_lots
            );
        if (eval_stats != nullptr) {
            ++eval_stats->prefix_cost_calls;
        }

        if (debug_compare_prefix_vs_linear) {
            CostForQuantityStats linear_stats;
            const auto linear = cost_for_quantity(curve, qty_lots, &linear_stats);
            if (linear.ok != prefix.ok ||
                linear.total_cost_tick != prefix.total_cost_tick ||
                linear.vwap_tick != prefix.vwap_tick ||
                linear.worst_price_tick != prefix.worst_price_tick) {
                if (eval_stats != nullptr) {
                    ++eval_stats->prefix_linear_mismatch;
                }
            }
        }

        CostForQuantityResult result;
        result.ok = prefix.ok;
        result.quantity_lots = qty_lots;
        result.total_cost_tick = prefix.total_cost_tick;
        result.vwap_tick = prefix.vwap_tick;
        result.worst_price_tick = prefix.worst_price_tick;
        return result;
    }

    CostForQuantityStats cost_stats;
    const auto result = cost_for_quantity(curve, qty_lots, &cost_stats);
    if (eval_stats != nullptr) {
        ++eval_stats->linear_cost_calls;
        eval_stats->depth_levels_scanned += cost_stats.depth_levels_scanned;
        if (cost_stats.depth_levels_scanned >
            eval_stats->max_depth_levels_scanned) {
            eval_stats->max_depth_levels_scanned =
                cost_stats.depth_levels_scanned;
        }
    }
    return result;
}

[[nodiscard]] CostForQuantityResult cached_cost_for_quantity_recorded(
    OrderDecisionCache* cache,
    std::uint64_t candidate_set_hash,
    std::uint8_t leg_index,
    const CostCurve& curve,
    const state::MarketDepthView* depth,
    std::int64_t bundle_qty,
    std::int64_t ratio_qty_lots,
    bool use_prefix_vwap,
    bool debug_compare_prefix_vs_linear,
    OrderDecisionEvalStats* eval_stats
) noexcept {
    std::int64_t leg_qty = 0;
    if (!checked_mul(ratio_qty_lots, bundle_qty, &leg_qty)) {
        return {};
    }

    if (cache != nullptr && depth != nullptr) {
        LegCostKey key;
        key.candidate_set_hash = candidate_set_hash;
        key.leg_index = leg_index;
        key.asset_index = depth->asset_index;
        key.book_version = depth->book_version;
        key.snapshot_version_hash = depth->snapshot_version_hash;
        key.depth_price_signature = depth_price_signature(*depth);
        key.q = bundle_qty;
        key.ratio_qty_lots = ratio_qty_lots;
        key.use_prefix_vwap = use_prefix_vwap;

        if (auto cached = cache->find_leg_cost(key); cached.has_value()) {
            if (eval_stats != nullptr) {
                ++eval_stats->leg_cost_cache_hits;
            }
            return *cached;
        }
        if (eval_stats != nullptr) {
            ++eval_stats->leg_cost_cache_misses;
        }

        auto result = cost_for_quantity_recorded(
            curve,
            depth,
            leg_qty,
            use_prefix_vwap,
            debug_compare_prefix_vs_linear,
            eval_stats
        );
        cache->store_leg_cost(key, result);
        return result;
    }

    return cost_for_quantity_recorded(
        curve,
        depth,
        leg_qty,
        use_prefix_vwap,
        debug_compare_prefix_vs_linear,
        eval_stats
    );
}

struct IncrementalEvalState {
    std::int64_t current_q = 0;
    std::uint16_t leg_count = 0;

    std::array<std::uint16_t, kMaxOrderDecisionLegs> level_index{};
    std::array<std::int64_t, kMaxOrderDecisionLegs> leg_qty_lots{};
    std::array<std::int64_t, kMaxOrderDecisionLegs> leg_cost_tick{};
    std::array<std::int64_t, kMaxOrderDecisionLegs> leg_worst_price_tick{};

    std::int64_t total_leg_cost_tick = 0;
};

[[nodiscard]] bool advance_incremental_leg(
    const CostCurve& curve,
    std::int64_t ratio_qty_lots,
    std::int64_t bundle_qty,
    std::uint16_t leg_index,
    IncrementalEvalState* state,
    OrderDecisionEvalStats* eval_stats
) noexcept {
    if (state == nullptr || leg_index >= state->leg_count ||
        ratio_qty_lots <= 0 || bundle_qty < state->current_q) {
        return false;
    }

    std::int64_t target_qty = 0;
    if (!checked_mul(ratio_qty_lots, bundle_qty, &target_qty)) {
        return false;
    }
    if (target_qty < state->leg_qty_lots[leg_index] ||
        target_qty > curve.total_qty_lots) {
        return false;
    }

    auto qty = state->leg_qty_lots[leg_index];
    auto cost = state->leg_cost_tick[leg_index];
    auto total_cost = state->total_leg_cost_tick;
    auto level_index = state->level_index[leg_index];
    auto worst_price = state->leg_worst_price_tick[leg_index];

    while (level_index < curve.level_count &&
           qty >= curve.levels[level_index].cumulative_qty_lots) {
        ++level_index;
        if (eval_stats != nullptr) {
            ++eval_stats->incremental_level_advances;
        }
    }

    while (qty < target_qty) {
        if (level_index >= curve.level_count) {
            return false;
        }

        const auto& level = curve.levels[level_index];
        const auto remaining_in_level = level.cumulative_qty_lots - qty;
        const auto needed = target_qty - qty;
        if (remaining_in_level <= 0 || needed <= 0 || level.price_tick <= 0) {
            return false;
        }

        const auto take = std::min(remaining_in_level, needed);
        std::int64_t cost_delta = 0;
        if (!checked_mul(level.price_tick, take, &cost_delta) ||
            !checked_add_to(cost_delta, &cost) ||
            !checked_add_to(cost_delta, &total_cost)) {
            return false;
        }

        qty += take;
        worst_price = level.price_tick;
        if (eval_stats != nullptr) {
            ++eval_stats->incremental_leg_updates;
        }

        if (qty >= level.cumulative_qty_lots) {
            ++level_index;
            if (eval_stats != nullptr) {
                ++eval_stats->incremental_level_advances;
            }
        }
    }

    state->level_index[leg_index] = level_index;
    state->leg_qty_lots[leg_index] = qty;
    state->leg_cost_tick[leg_index] = cost;
    state->leg_worst_price_tick[leg_index] = worst_price;
    state->total_leg_cost_tick = total_cost;
    return true;
}

enum class IncrementalAdvanceStatus : std::uint8_t {
    Ok,
    PartialFillRisk,
    Invalid
};

[[nodiscard]] IncrementalAdvanceStatus advance_incremental_state(
    const std::array<CostCurve, kMaxOrderDecisionLegs>& curves,
    const std::array<std::int64_t, kMaxOrderDecisionLegs>& ratios,
    std::uint16_t leg_count,
    const risk::RiskPolicySnapshot& policy,
    std::int64_t bundle_qty,
    IncrementalEvalState* state,
    OrderDecisionEvalStats* eval_stats
) noexcept {
    if (state == nullptr || bundle_qty < state->current_q) {
        return IncrementalAdvanceStatus::Invalid;
    }

    auto next = *state;
    next.leg_count = leg_count;
    for (std::uint16_t i = 0; i < leg_count; ++i) {
        std::int64_t leg_qty = 0;
        if (!checked_mul(ratios[i], bundle_qty, &leg_qty)) {
            return IncrementalAdvanceStatus::Invalid;
        }

        const auto required_depth =
            ceil_ratio_bps(leg_qty, policy.min_depth_margin_bps);
        if (curves[i].total_qty_lots < required_depth) {
            return IncrementalAdvanceStatus::PartialFillRisk;
        }

        if (!advance_incremental_leg(
                curves[i],
                ratios[i],
                bundle_qty,
                i,
                &next,
                eval_stats
            )) {
            return IncrementalAdvanceStatus::Invalid;
        }
    }

    next.current_q = bundle_qty;
    *state = next;
    if (eval_stats != nullptr) {
        ++eval_stats->incremental_eval_steps;
    }
    return IncrementalAdvanceStatus::Ok;
}

}  // namespace

BundleSizeResult BundleSizeOptimizer::optimize(
    const BundleSizeInput& input
) const {
    OrderDecisionStageTimings timings;
    OrderDecisionEvalStats eval_stats;

    auto validate_start = Clock::now();
    if (input.intent == nullptr || input.bundle == nullptr ||
        input.policy == nullptr) {
        timings.depth_view_prepare_ns += elapsed_ns(validate_start);
        return reject(OrderDecisionType::RejectInvalidBundle, timings, eval_stats);
    }
    const auto& intent = *input.intent;
    const auto& bundle = *input.bundle;
    const auto& policy = *input.policy;

    if (bundle.leg_count == 0 || bundle.leg_count > kMaxOrderDecisionLegs) {
        timings.depth_view_prepare_ns += elapsed_ns(validate_start);
        return reject(OrderDecisionType::RejectInvalidBundle, timings, eval_stats);
    }
    if (intent.leg_count != 0 && intent.leg_count < bundle.leg_count) {
        timings.depth_view_prepare_ns += elapsed_ns(validate_start);
        return reject(OrderDecisionType::RejectInvalidBundle, timings, eval_stats);
    }
    timings.depth_view_prepare_ns += elapsed_ns(validate_start);

    std::array<CostCurve, kMaxOrderDecisionLegs> curves{};
    std::array<std::int64_t, kMaxOrderDecisionLegs> ratios{};
    std::array<std::uint32_t, kMaxOrderDecisionLegs> asset_indices{};
    std::array<const state::MarketDepthView*, kMaxOrderDecisionLegs> depths{};

    const CostCurveBuilder curve_builder;
    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        auto depth_prepare_start = Clock::now();
        const auto& bundle_leg = bundle.legs[i];
        const auto* intent_leg = intent.leg_count > i ? &intent.legs[i] : nullptr;
        if (bundle_leg.side != Side::Buy) {
            timings.depth_view_prepare_ns += elapsed_ns(depth_prepare_start);
            return reject(
                OrderDecisionType::RejectUnsupportedSide,
                timings,
                eval_stats
            );
        }

        ratios[i] = ratio_for_leg(bundle_leg, intent_leg);
        if (ratios[i] <= 0) {
            timings.depth_view_prepare_ns += elapsed_ns(depth_prepare_start);
            return reject(
                OrderDecisionType::RejectInvalidBundle,
                timings,
                eval_stats
            );
        }

        asset_indices[i] =
            intent_leg != nullptr ? intent_leg->asset_index : 0U;
        const auto* depth = find_depth_view(input.depth_views, asset_indices[i], i);
        if (depth == nullptr || !depth_flags_usable(*depth)) {
            timings.depth_view_prepare_ns += elapsed_ns(depth_prepare_start);
            return reject(OrderDecisionType::RejectNoDepth, timings, eval_stats);
        }
        asset_indices[i] = depth->asset_index;
        depths[i] = depth;
        timings.depth_view_prepare_ns += elapsed_ns(depth_prepare_start);

        const auto curve_start = Clock::now();
        const auto curve_result = curve_builder.build_buy_curve({
            .leg = &bundle_leg,
            .depth = depth,
            .asset_index = asset_indices[i]
        });
        timings.cost_curve_build_ns += elapsed_ns(curve_start);
        if (!curve_result.ok) {
            return reject(curve_result.reject_reason, timings, eval_stats);
        }
        curves[i] = curve_result.curve;
    }

    const auto candidate_prepare_start = Clock::now();
    const auto min_bundle_qty = std::max<std::int64_t>(
        1,
        input.config.min_bundle_qty
    );
    timings.depth_view_prepare_ns += elapsed_ns(candidate_prepare_start);
    const auto curve_span =
        std::span<const CostCurve>(curves.data(), bundle.leg_count);
    const auto breakpoint_signature =
        depth_breakpoint_signature(curve_span, ratios, depths, bundle.leg_count);
    const auto candidate_key = make_candidate_set_key(
        intent,
        bundle,
        breakpoint_signature,
        input.config
    );

    const CandidateSet* cached_candidate_set = nullptr;
    if (input.cache != nullptr) {
        cached_candidate_set = input.cache->find_candidate_set(candidate_key);
        if (cached_candidate_set != nullptr) {
            ++eval_stats.candidate_set_cache_hits;
        } else {
            ++eval_stats.candidate_set_cache_misses;
        }
    }

    std::span<const std::int64_t> candidate_values;
    if (cached_candidate_set != nullptr) {
        candidate_values = std::span<const std::int64_t>{
            cached_candidate_set->q_values.data(),
            cached_candidate_set->count
        };
        eval_stats.candidate_count =
            static_cast<std::uint32_t>(cached_candidate_set->count);
    }

    const auto candidate_eval_start = Clock::now();
    const auto guaranteed_per_bundle =
        bundle.guaranteed_payout_tick != 0
            ? bundle.guaranteed_payout_tick
            : intent.guaranteed_payout_tick;

    bool saw_budget_reject = false;
    bool saw_partial_reject = false;
    bool candidates_exhausted = false;
    bool found = false;
    EdgeCurvePoint best;
    IncrementalEvalState incremental_state;
    incremental_state.leg_count = bundle.leg_count;
    IncrementalEvalState best_state;

    auto evaluate_candidate = [&](std::int64_t q) {
        if (q < min_bundle_qty) {
            return;
        }
        ++eval_stats.candidates_evaluated;

        const auto advance_status = advance_incremental_state(
            curves,
            ratios,
            bundle.leg_count,
            policy,
            q,
            &incremental_state,
            &eval_stats
        );
        if (advance_status == IncrementalAdvanceStatus::PartialFillRisk) {
            saw_partial_reject = true;
            candidates_exhausted = true;
            return;
        }
        if (advance_status != IncrementalAdvanceStatus::Ok) {
            return;
        }

        std::int64_t fee = 0;
        std::int64_t latency = 0;
        std::int64_t slippage = 0;
        if (!checked_mul(input.config.fee_per_bundle_tick, q, &fee) ||
            !checked_mul(
                input.config.latency_buffer_per_bundle_tick,
                q,
                &latency
            ) ||
            !checked_mul(
                input.config.slippage_buffer_per_bundle_tick,
                q,
                &slippage
            )) {
            return;
        }

        std::int64_t total_cost = incremental_state.total_leg_cost_tick;
        if (!checked_add_to(fee, &total_cost) ||
            !checked_add_to(latency, &total_cost) ||
            !checked_add_to(slippage, &total_cost)) {
            return;
        }
        if (policy.max_total_cost_tick > 0 &&
            total_cost > policy.max_total_cost_tick) {
            saw_budget_reject = true;
            return;
        }

        std::int64_t payout = 0;
        if (!checked_mul(guaranteed_per_bundle, q, &payout)) {
            return;
        }

        std::int64_t total_edge = 0;
        if (!common::math::checked_sub_i64(payout, total_cost, &total_edge)) {
            return;
        }
        const auto unit_edge = total_edge / q;
        const auto edge_bps = common::math::ratio_bps(total_edge, total_cost);

        bool rejected_by_edge = false;
        if (total_edge < policy.min_post_risk_total_edge_tick) {
            ++eval_stats.rejected_by_total_edge;
            rejected_by_edge = true;
        }
        if (unit_edge < policy.min_post_risk_unit_edge_tick) {
            ++eval_stats.rejected_by_unit_edge;
            rejected_by_edge = true;
        }
        if (edge_bps < policy.min_edge_bps) {
            ++eval_stats.rejected_by_bps;
            rejected_by_edge = true;
        }
        if (rejected_by_edge) {
            return;
        }

        EdgeCurvePoint point;
        point.bundle_qty = q;
        point.total_cost_tick = total_cost;
        point.unit_edge_tick = unit_edge;
        point.total_edge_tick = total_edge;
        point.edge_bps = edge_bps;

        if (!found || better_edge_candidate(point, best)) {
            found = true;
            best = point;
            best_state = incremental_state;
        }
    };

    if (cached_candidate_set != nullptr) {
        if (candidate_values.empty()) {
            timings.candidate_eval_ns += elapsed_ns(candidate_eval_start);
            return reject(OrderDecisionType::RejectNoDepth, timings, eval_stats);
        }
        for (const auto q : candidate_values) {
            evaluate_candidate(q);
            if (candidates_exhausted) {
                break;
            }
        }
    } else {
        std::array<std::int64_t, kMaxOrderDecisionCandidates> streamed_candidates{};
        std::uint16_t streamed_candidate_count = 0;
        SizeCandidateEnumerationInput enumeration_input{
            .curves = curve_span,
            .ratio_qty_lots = ratios,
            .depth_views = depths,
            .leg_count = bundle.leg_count,
            .min_bundle_qty = min_bundle_qty,
            .max_bundle_qty = input.config.max_bundle_qty,
            // Budget depends on prices, so it is evaluated below instead of
            // being baked into the reusable breakpoint cache.
            .max_total_cost_tick = 0,
            .use_prefix_vwap = input.config.use_prefix_vwap,
            .debug_compare_prefix_vs_linear =
                input.config.debug_compare_prefix_vs_linear
        };

        KWayCandidateCursor cursor(enumeration_input);
        std::int64_t q = 0;
        while (cursor.next(&q, &eval_stats)) {
            if (streamed_candidate_count < streamed_candidates.size()) {
                streamed_candidates[streamed_candidate_count++] = q;
            }
            ++eval_stats.candidate_count;
            evaluate_candidate(q);
            if (candidates_exhausted) {
                break;
            }
        }
        timings.candidate_sort_dedup_ns = 0;

        if (input.cache != nullptr && streamed_candidate_count > 0) {
            cached_candidate_set = &input.cache->store_candidate_set(
                candidate_key,
                std::span<const std::int64_t>{
                    streamed_candidates.data(),
                    streamed_candidate_count
                }
            );
        }
    }

    if (!found) {
        timings.candidate_eval_ns += elapsed_ns(candidate_eval_start);
        if (eval_stats.candidate_count == 0) {
            return reject(OrderDecisionType::RejectNoDepth, timings, eval_stats);
        }
        if (saw_budget_reject) {
            return reject(
                OrderDecisionType::RejectRiskBudget,
                timings,
                eval_stats
            );
        }
        if (saw_partial_reject) {
            return reject(
                OrderDecisionType::RejectPartialFillRisk,
                timings,
                eval_stats
            );
        }
        return reject(OrderDecisionType::RejectLowEdge, timings, eval_stats);
    }

    std::array<OrderDecisionLegLite, kMaxOrderDecisionLegs> best_legs{};
    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        const auto leg_qty = best_state.leg_qty_lots[i];
        const auto leg_cost = best_state.leg_cost_tick[i];
        if (leg_qty <= 0 || leg_cost < 0) {
            timings.candidate_eval_ns += elapsed_ns(candidate_eval_start);
            return reject(OrderDecisionType::RejectNoDepth, timings, eval_stats);
        }

        auto& out_leg = best_legs[i];
        out_leg.asset_index = asset_indices[i];
        out_leg.side = Side::Buy;
        out_leg.quantity_lots = leg_qty;
        out_leg.estimated_vwap_tick = leg_cost / leg_qty;
        out_leg.worst_price_tick = best_state.leg_worst_price_tick[i];
        out_leg.estimated_cost_tick = leg_cost;
    }
    timings.candidate_eval_ns += elapsed_ns(candidate_eval_start);

    BundleSizeResult result;
    result.ok = true;
    result.best_bundle_qty = best.bundle_qty;
    result.guaranteed_payout_tick = guaranteed_per_bundle * best.bundle_qty;
    result.total_cost_tick = best.total_cost_tick;
    result.unit_edge_tick = best.unit_edge_tick;
    result.total_edge_tick = best.total_edge_tick;
    result.edge_bps = best.edge_bps;
    result.leg_count = bundle.leg_count;
    result.legs = best_legs;
    result.reject_reason = OrderDecisionType::NoTrade;
    result.timings = timings;
    result.eval_stats = eval_stats;
    return result;
}

}  // namespace trading_engine::order_decision
