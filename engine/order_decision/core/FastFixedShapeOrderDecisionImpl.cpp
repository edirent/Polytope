#include "engine/order_decision/core/FastFixedShapeOrderDecisionImpl.h"

#include "engine/common/math/FixedPointMath.h"
#include "engine/common/math/RiskMath.h"
#include "engine/common/math/VwapMath.h"
#include "engine/order_decision/limits/LimitPriceBuilder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <utility>

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

[[nodiscard]] OrderDecisionResult reject(
    OrderDecisionType reason,
    std::string error,
    const OrderDecisionStageTimings& timings,
    const OrderDecisionEvalStats& eval_stats = {}
) {
    OrderDecisionResult result;
    result.reject_reason = reason;
    result.error = std::move(error);
    result.decision.type = reason;
    result.timings = timings;
    result.eval_stats = eval_stats;
    return result;
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
    if (asset_index != 0) {
        for (const auto& view : depth_views) {
            if (view.asset_index == asset_index) {
                return &view;
            }
        }
    }
    if (fallback_index < depth_views.size()) {
        return &depth_views[fallback_index];
    }
    return nullptr;
}

[[nodiscard]] bool depth_usable(
    const state::MarketDepthView& depth,
    const risk::RiskPolicySnapshot& policy,
    std::uint64_t now_ns
) noexcept {
    if (!depth.usable_for_depth || depth.recovering || depth.crossed ||
        depth.closed || depth.resolved || depth.ask_count == 0) {
        return false;
    }
    if (policy.max_book_age_ns > 0 && depth.last_ws_recv_ns != 0 &&
        now_ns > depth.last_ws_recv_ns &&
        now_ns - depth.last_ws_recv_ns >
            static_cast<std::uint64_t>(policy.max_book_age_ns)) {
        return false;
    }
    return true;
}

[[nodiscard]] std::int64_t executable_ask_qty(
    const state::MarketDepthView& depth
) noexcept {
    if (depth.prefix.ask_count > 0) {
        return state::ask_depth_from_prefix(depth.prefix);
    }
    std::int64_t total = 0;
    const auto count = std::min<std::uint16_t>(
        depth.ask_count,
        state::kMaxSnapshotDepth
    );
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto& level = depth.asks[i];
        if (level.price_tick <= 0) {
            continue;
        }
        if (!checked_add_to(common::math::price_level_size_lots(level), &total)) {
            return std::numeric_limits<std::int64_t>::max();
        }
    }
    return total;
}

[[nodiscard]] common::math::VwapMathResult buy_vwap(
    const state::MarketDepthView& depth,
    std::int64_t quantity_lots,
    OrderDecisionEvalStats* stats
) noexcept {
    if (stats != nullptr) {
        ++stats->vwap_cost_calls;
    }
    if (depth.prefix.ask_count > 0) {
        if (stats != nullptr) {
            ++stats->prefix_cost_calls;
        }
        return common::math::buy_vwap_prefix(
            depth.prefix,
            depth.asks.data(),
            depth.ask_count,
            quantity_lots
        );
    }
    if (stats != nullptr) {
        ++stats->linear_cost_calls;
        stats->depth_levels_scanned += depth.ask_count;
        stats->max_depth_levels_scanned =
            std::max<std::uint32_t>(
                stats->max_depth_levels_scanned,
                depth.ask_count
            );
    }
    return common::math::buy_vwap_linear(
        depth.asks.data(),
        depth.ask_count,
        quantity_lots
    );
}

[[nodiscard]] std::int64_t multiply_or_zero(
    std::int64_t lhs,
    std::int64_t rhs
) noexcept {
    std::int64_t out = 0;
    if (!checked_mul(lhs, rhs, &out)) {
        return 0;
    }
    return out;
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

}  // namespace

OrderDecisionResult FastFixedShapeOrderDecisionImpl::decide(
    const signal::OpportunityIntent& intent,
    const oracle::CandidateBundle& bundle,
    std::span<const state::MarketDepthView> depth_views,
    const risk::RiskPolicySnapshot& policy,
    std::uint64_t now_ns,
    const OrderDecisionConfig& config,
    OrderDecisionCache* cache
) const {
    const auto total_start = Clock::now();
    OrderDecisionStageTimings timings;
    OrderDecisionEvalStats eval_stats;

    auto finish = [&](OrderDecisionResult result) {
        timings.total_ns = elapsed_ns(total_start);
        finalize_stage_timings(&timings);
        result.timings = timings;
        return result;
    };

    const auto validate_start = Clock::now();
    if (intent.expires_at_ns != 0 && intent.expires_at_ns <= now_ns) {
        timings.input_validate_ns += elapsed_ns(validate_start);
        return finish(reject(
            OrderDecisionType::RejectExpiredIntent,
            "intent expired",
            timings,
            eval_stats
        ));
    }
    if (intent.status != signal::IntentStatus::PaperOpportunity) {
        timings.input_validate_ns += elapsed_ns(validate_start);
        return finish(reject(
            OrderDecisionType::RejectInvalidBundle,
            "intent is not PaperOpportunity",
            timings,
            eval_stats
        ));
    }
    if (bundle.bundle_id == 0 || intent.bundle_id != bundle.bundle_id ||
        bundle.leg_count == 0 || bundle.leg_count > kMaxOrderDecisionLegs) {
        timings.input_validate_ns += elapsed_ns(validate_start);
        return finish(reject(
            OrderDecisionType::RejectInvalidBundle,
            "invalid bundle",
            timings,
            eval_stats
        ));
    }
    timings.input_validate_ns += elapsed_ns(validate_start);

    const auto memo_key = make_decision_memo_key(
        intent,
        bundle,
        policy,
        config,
        now_ns
    );
    if (cache != nullptr) {
        if (auto memo = cache->find_decision(memo_key); memo.has_value()) {
            OrderDecisionResult result;
            result.ok = true;
            result.reject_reason = OrderDecisionType::NoTrade;
            result.decision = std::move(*memo);
            result.eval_stats.decision_memo_cache_hits = 1;
            return finish(std::move(result));
        }
    }

    bool spec_cache_hit = false;
    const auto spec = cache != nullptr
        ? cache->fixed_shape_spec(intent, bundle, &spec_cache_hit)
        : FixedShapeOrderDecisionSpec{};
    if (cache != nullptr) {
        if (spec_cache_hit) {
            ++eval_stats.fixed_shape_spec_cache_hits;
        } else {
            ++eval_stats.fixed_shape_spec_cache_misses;
        }
    }

    std::array<const state::MarketDepthView*, kMaxOrderDecisionLegs> depths{};
    std::array<std::int64_t, kMaxOrderDecisionLegs> ratios{};
    std::array<std::int64_t, kMaxOrderDecisionLegs> executable_qty{};

    std::int64_t bundle_qty = std::numeric_limits<std::int64_t>::max();
    const auto depth_start = Clock::now();
    const auto leg_count = cache != nullptr ? spec.leg_count : bundle.leg_count;
    for (std::uint16_t i = 0; i < leg_count; ++i) {
        const auto& bundle_leg = bundle.legs[i];
        const auto* intent_leg = intent.leg_count > i ? &intent.legs[i] : nullptr;
        const auto side = cache != nullptr ? spec.sides[i] : bundle_leg.side;
        if (side != Side::Buy) {
            timings.depth_view_prepare_ns += elapsed_ns(depth_start);
            return finish(reject(
                OrderDecisionType::RejectUnsupportedSide,
                "SELL legs are unsupported in fast fixed-shape v1",
                timings,
                eval_stats
            ));
        }

        ratios[i] =
            cache != nullptr ? spec.ratio_qty_lots[i]
                             : ratio_for_leg(bundle_leg, intent_leg);
        const auto asset_index =
            cache != nullptr ? spec.asset_indices[i]
                             : (intent_leg != nullptr ? intent_leg->asset_index
                                                      : 0U);
        const auto* depth = find_depth_view(depth_views, asset_index, i);
        if (ratios[i] <= 0 || depth == nullptr ||
            !depth_usable(*depth, policy, now_ns)) {
            timings.depth_view_prepare_ns += elapsed_ns(depth_start);
            return finish(reject(
                OrderDecisionType::RejectNoDepth,
                "missing or unusable depth",
                timings,
                eval_stats
            ));
        }
        depths[i] = depth;
        executable_qty[i] = executable_ask_qty(*depth);
        if (executable_qty[i] < ratios[i]) {
            timings.depth_view_prepare_ns += elapsed_ns(depth_start);
            return finish(reject(
                OrderDecisionType::RejectNoDepth,
                "insufficient executable depth",
                timings,
                eval_stats
            ));
        }
        bundle_qty = std::min(bundle_qty, executable_qty[i] / ratios[i]);
    }
    if (config.max_bundle_qty > 0) {
        bundle_qty = std::min(bundle_qty, config.max_bundle_qty);
    }
    const auto min_bundle_qty = std::max<std::int64_t>(1, config.min_bundle_qty);
    if (bundle_qty < min_bundle_qty ||
        bundle_qty == std::numeric_limits<std::int64_t>::max()) {
        timings.depth_view_prepare_ns += elapsed_ns(depth_start);
        return finish(reject(
            OrderDecisionType::RejectNoDepth,
            "no executable bundle quantity",
            timings,
            eval_stats
        ));
    }
    timings.depth_view_prepare_ns += elapsed_ns(depth_start);

    eval_stats.candidate_count = 1;
    eval_stats.candidates_evaluated = 1;
    const auto eval_start = Clock::now();
    std::array<OrderDecisionLegLite, kMaxOrderDecisionLegs> legs{};
    std::int64_t total_leg_cost = 0;
    bool saw_partial_reject = false;
    for (std::uint16_t i = 0; i < leg_count; ++i) {
        std::int64_t leg_qty = 0;
        if (!checked_mul(ratios[i], bundle_qty, &leg_qty)) {
            timings.candidate_eval_ns += elapsed_ns(eval_start);
            return finish(reject(
                OrderDecisionType::RejectInternalError,
                "quantity overflow",
                timings,
                eval_stats
            ));
        }
        const auto required_depth =
            ceil_ratio_bps(leg_qty, policy.min_depth_margin_bps);
        if (executable_qty[i] < required_depth) {
            saw_partial_reject = true;
            break;
        }
        const auto priced = buy_vwap(*depths[i], leg_qty, &eval_stats);
        if (!priced.ok ||
            !checked_add_to(priced.total_cost_tick, &total_leg_cost)) {
            timings.candidate_eval_ns += elapsed_ns(eval_start);
            return finish(reject(
                OrderDecisionType::RejectNoDepth,
                "failed to price executable depth",
                timings,
                eval_stats
            ));
        }

        auto& out = legs[i];
        out.asset_index = depths[i]->asset_index;
        out.side = Side::Buy;
        out.quantity_lots = leg_qty;
        out.estimated_vwap_tick = priced.vwap_tick;
        out.worst_price_tick = priced.worst_price_tick;
        out.estimated_cost_tick = priced.total_cost_tick;
    }
    if (saw_partial_reject) {
        timings.candidate_eval_ns += elapsed_ns(eval_start);
        return finish(reject(
            OrderDecisionType::RejectPartialFillRisk,
            "depth margin failed",
            timings,
            eval_stats
        ));
    }

    const auto fee = multiply_or_zero(config.fee_per_bundle_tick, bundle_qty);
    const auto latency =
        multiply_or_zero(config.latency_buffer_per_bundle_tick, bundle_qty);
    const auto slippage =
        multiply_or_zero(config.slippage_buffer_per_bundle_tick, bundle_qty);
    std::int64_t total_cost = total_leg_cost;
    if (!checked_add_to(fee, &total_cost) ||
        !checked_add_to(latency, &total_cost) ||
        !checked_add_to(slippage, &total_cost)) {
        timings.candidate_eval_ns += elapsed_ns(eval_start);
        return finish(reject(
            OrderDecisionType::RejectInternalError,
            "cost overflow",
            timings,
            eval_stats
        ));
    }
    if (policy.max_total_cost_tick > 0 && total_cost > policy.max_total_cost_tick) {
        timings.candidate_eval_ns += elapsed_ns(eval_start);
        return finish(reject(
            OrderDecisionType::RejectRiskBudget,
            "cost exceeds policy budget",
            timings,
            eval_stats
        ));
    }

    const auto guaranteed_per_bundle = cache != nullptr
        ? spec.guaranteed_payout_per_bundle_tick
        : (bundle.guaranteed_payout_tick != 0 ? bundle.guaranteed_payout_tick
                                              : intent.guaranteed_payout_tick);
    std::int64_t guaranteed_payout = 0;
    if (!checked_mul(guaranteed_per_bundle, bundle_qty, &guaranteed_payout)) {
        timings.candidate_eval_ns += elapsed_ns(eval_start);
        return finish(reject(
            OrderDecisionType::RejectInternalError,
            "payout overflow",
            timings,
            eval_stats
        ));
    }
    std::int64_t total_edge = 0;
    if (!common::math::checked_sub_i64(
            guaranteed_payout,
            total_cost,
            &total_edge
        )) {
        timings.candidate_eval_ns += elapsed_ns(eval_start);
        return finish(reject(
            OrderDecisionType::RejectInternalError,
            "edge overflow",
            timings,
            eval_stats
        ));
    }
    const auto unit_edge = total_edge / bundle_qty;
    const auto edge_bps = common::math::ratio_bps(total_edge, total_cost);
    bool edge_rejected = false;
    if (unit_edge < policy.min_post_risk_unit_edge_tick) {
        ++eval_stats.rejected_by_unit_edge;
        edge_rejected = true;
    }
    if (total_edge < policy.min_post_risk_total_edge_tick) {
        ++eval_stats.rejected_by_total_edge;
        edge_rejected = true;
    }
    if (edge_bps < policy.min_edge_bps) {
        ++eval_stats.rejected_by_bps;
        edge_rejected = true;
    }
    if (edge_rejected) {
        timings.candidate_eval_ns += elapsed_ns(eval_start);
        return finish(reject(
            OrderDecisionType::RejectLowEdge,
            "edge below policy threshold",
            timings,
            eval_stats
        ));
    }
    timings.candidate_eval_ns += elapsed_ns(eval_start);

    const auto build_start = Clock::now();
    OrderDecisionLite decision;
    decision.source_intent_id = intent.intent_id;
    decision.bundle_id = bundle.bundle_id;
    decision.type = OrderDecisionType::PaperOrderDecision;
    decision.chosen_bundle_qty = bundle_qty;
    decision.guaranteed_payout_tick = guaranteed_payout;
    decision.estimated_total_cost_tick = total_cost;
    decision.estimated_fee_tick = fee;
    decision.latency_buffer_tick = latency;
    decision.slippage_buffer_tick = slippage;
    decision.unit_edge_tick = unit_edge;
    decision.total_edge_tick = total_edge;
    decision.edge_bps = edge_bps;
    decision.leg_count = leg_count;
    decision.legs = legs;
    decision.snapshot_version_hash = intent.snapshot_version_hash != 0
        ? intent.snapshot_version_hash
        : state::hash_depth_views(depth_views);
    decision.oracle_artifact_hash = intent.oracle_artifact_hash;
    decision.bundle_hash = intent.bundle_hash;
    decision.policy_hash = policy.policy_hash != 0
        ? policy.policy_hash
        : risk::compute_policy_hash(policy);
    decision.created_ts_ns = now_ns;
    const auto default_expiry = now_ns + config.default_ttl_ns;
    decision.expires_at_ns = intent.expires_at_ns != 0
        ? std::min(intent.expires_at_ns, default_expiry)
        : default_expiry;
    timings.decision_build_ns += elapsed_ns(build_start);

    const LimitPriceBuilder limit_builder;
    for (std::uint16_t i = 0; i < decision.leg_count; ++i) {
        const auto limit_start = Clock::now();
        const auto limit = limit_builder.build_buy_limit({
            .worst_consumed_price_tick = decision.legs[i].worst_price_tick,
            .protection_buffer_tick = config.price_protection_buffer_tick,
            .leg_max_price_tick = bundle.legs[i].max_price_tick,
            .configured_max_price_tick = config.max_allowed_price_tick
        });
        timings.limit_price_build_ns += elapsed_ns(limit_start);
        if (!limit.ok) {
            return finish(reject(
                limit.reject_reason,
                to_string(limit.reject_reason),
                timings,
                eval_stats
            ));
        }
        decision.legs[i].limit_price_tick = limit.limit_price_tick;
    }

    const auto hash_start = Clock::now();
    decision.decision_hash = compute_order_decision_hash(decision);
    decision.decision_id = decision.decision_hash;
    timings.decision_hash_ns += elapsed_ns(hash_start);

    OrderDecisionResult result;
    result.ok = true;
    result.decision = decision;
    result.reject_reason = OrderDecisionType::NoTrade;
    result.eval_stats = eval_stats;
    if (cache != nullptr) {
        ++result.eval_stats.decision_memo_cache_misses;
        cache->store_decision(memo_key, result.decision);
    }
    return finish(std::move(result));
}

}  // namespace trading_engine::order_decision
