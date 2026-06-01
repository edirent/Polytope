#include "engine/order_decision/core/GenericOrderDecisionImpl.h"

#include "engine/common/math/FixedPointMath.h"
#include "engine/order_decision/limits/LimitPriceBuilder.h"
#include "engine/order_decision/sizing/BundleSizeOptimizer.h"

#include <algorithm>
#include <chrono>
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
    std::string error
) {
    OrderDecisionResult result;
    result.reject_reason = reason;
    result.error = std::move(error);
    result.decision.type = reason;
    return result;
}

[[nodiscard]] std::int64_t multiply_or_zero(
    std::int64_t lhs,
    std::int64_t rhs
) noexcept {
    std::int64_t out = 0;
    if (!common::math::checked_mul_i64(lhs, rhs, &out)) {
        return 0;
    }
    return out;
}

}  // namespace

OrderDecisionResult GenericOrderDecisionImpl::decide(
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

    auto finish = [&](OrderDecisionResult result) {
        timings.total_ns = elapsed_ns(total_start);
        finalize_stage_timings(&timings);
        result.timings = timings;
        return result;
    };

    auto validate_start = Clock::now();
    if (intent.expires_at_ns != 0 && intent.expires_at_ns <= now_ns) {
        timings.input_validate_ns += elapsed_ns(validate_start);
        return finish(
            reject(OrderDecisionType::RejectExpiredIntent, "intent expired")
        );
    }
    if (intent.status != signal::IntentStatus::PaperOpportunity) {
        timings.input_validate_ns += elapsed_ns(validate_start);
        return finish(
            reject(
                OrderDecisionType::RejectInvalidBundle,
                "intent is not PaperOpportunity"
            )
        );
    }
    if (bundle.bundle_id == 0 || intent.bundle_id != bundle.bundle_id ||
        bundle.leg_count == 0 || bundle.leg_count > kMaxOrderDecisionLegs) {
        timings.input_validate_ns += elapsed_ns(validate_start);
        return finish(
            reject(OrderDecisionType::RejectInvalidBundle, "invalid bundle")
        );
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

    const BundleSizeOptimizer optimizer;
    auto size = optimizer.optimize({
        .intent = &intent,
        .bundle = &bundle,
        .depth_views = depth_views,
        .policy = &policy,
        .config = config,
        .cache = cache
    });
    add_stage_timings(&timings, size.timings);
    if (!size.ok) {
        auto result = reject(size.reject_reason, to_string(size.reject_reason));
        result.eval_stats = size.eval_stats;
        return finish(std::move(result));
    }

    const auto decision_build_start = Clock::now();
    OrderDecisionLite decision;
    decision.source_intent_id = intent.intent_id;
    decision.bundle_id = bundle.bundle_id;
    decision.type = OrderDecisionType::PaperOrderDecision;
    decision.chosen_bundle_qty = size.best_bundle_qty;
    decision.guaranteed_payout_tick = size.guaranteed_payout_tick;
    decision.estimated_total_cost_tick = size.total_cost_tick;
    decision.estimated_fee_tick =
        multiply_or_zero(config.fee_per_bundle_tick, size.best_bundle_qty);
    decision.latency_buffer_tick = multiply_or_zero(
        config.latency_buffer_per_bundle_tick,
        size.best_bundle_qty
    );
    decision.slippage_buffer_tick = multiply_or_zero(
        config.slippage_buffer_per_bundle_tick,
        size.best_bundle_qty
    );
    decision.unit_edge_tick = size.unit_edge_tick;
    decision.total_edge_tick = size.total_edge_tick;
    decision.edge_bps = size.edge_bps;
    decision.leg_count = size.leg_count;
    decision.legs = size.legs;
    decision.snapshot_version_hash = intent.snapshot_version_hash;
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
    timings.decision_build_ns += elapsed_ns(decision_build_start);

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
            return finish(
                reject(
                    limit.reject_reason,
                    to_string(limit.reject_reason)
                )
            );
        }
        decision.legs[i].limit_price_tick = limit.limit_price_tick;
    }

    const auto hash_start = Clock::now();
    decision.decision_hash = compute_order_decision_hash(decision);
    decision.decision_id = decision.decision_hash;
    timings.decision_hash_ns += elapsed_ns(hash_start);

    OrderDecisionResult result;
    result.ok = true;
    result.reject_reason = OrderDecisionType::NoTrade;
    result.decision = decision;
    result.eval_stats = size.eval_stats;
    if (cache != nullptr) {
        ++result.eval_stats.decision_memo_cache_misses;
        cache->store_decision(memo_key, result.decision);
    }
    return finish(std::move(result));
}

}  // namespace trading_engine::order_decision
