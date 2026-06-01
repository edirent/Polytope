#include "engine/order_decision/core/OrderDecisionCache.h"

#include "engine/common/math/VwapMath.h"
#include "engine/order_decision/math/OrderDecisionMath.h"

#include <algorithm>

namespace trading_engine::order_decision {

namespace {

[[nodiscard]] std::uint64_t nonzero_hash(std::uint64_t hash) noexcept {
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] std::uint64_t side_value(Side side) noexcept {
    return static_cast<std::uint64_t>(side);
}

[[nodiscard]] std::uint64_t spec_cache_key(
    const signal::OpportunityIntent& intent,
    const oracle::CandidateBundle& bundle
) noexcept {
    auto hash = kOrderDecisionFnvOffset;
    mix_u64(&hash, bundle.bundle_id);
    mix_u64(&hash, intent.bundle_hash);
    mix_u64(&hash, intent.oracle_artifact_hash);
    mix_u64(&hash, intent.constraint_hash);
    mix_u64(&hash, bundle.leg_count);
    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        const auto& leg = bundle.legs[i];
        mix_u64(&hash, side_value(leg.side));
        mix_i64(&hash, leg.quantity_lots);
        if (intent.leg_count > i) {
            mix_u64(&hash, intent.legs[i].asset_index);
        }
        mix_string(&hash, leg.market_id);
        mix_string(&hash, leg.asset_id);
    }
    return nonzero_hash(hash);
}

[[nodiscard]] std::uint64_t hash_candidate_set(
    const CandidateSetKey& key,
    std::span<const std::int64_t> q_values
) noexcept {
    auto hash = kOrderDecisionFnvOffset;
    mix_u64(&hash, key.bundle_id);
    mix_u64(&hash, key.bundle_hash);
    mix_u64(&hash, key.depth_breakpoint_signature);
    mix_u64(&hash, key.config_hash);
    mix_u64(&hash, q_values.size());
    for (const auto q : q_values) {
        mix_i64(&hash, q);
    }
    return nonzero_hash(hash);
}

}  // namespace

std::size_t CandidateSetKeyHasher::operator()(
    const CandidateSetKey& key
) const noexcept {
    auto hash = kOrderDecisionFnvOffset;
    mix_u64(&hash, key.bundle_id);
    mix_u64(&hash, key.bundle_hash);
    mix_u64(&hash, key.depth_breakpoint_signature);
    mix_u64(&hash, key.config_hash);
    return static_cast<std::size_t>(nonzero_hash(hash));
}

std::size_t LegCostKeyHasher::operator()(
    const LegCostKey& key
) const noexcept {
    auto hash = kOrderDecisionFnvOffset;
    mix_u64(&hash, key.candidate_set_hash);
    mix_u64(&hash, key.leg_index);
    mix_u64(&hash, key.asset_index);
    mix_u64(&hash, key.book_version);
    mix_u64(&hash, key.snapshot_version_hash);
    mix_u64(&hash, key.depth_price_signature);
    mix_i64(&hash, key.q);
    mix_i64(&hash, key.ratio_qty_lots);
    mix_u64(&hash, key.use_prefix_vwap ? 1ULL : 0ULL);
    return static_cast<std::size_t>(nonzero_hash(hash));
}

std::size_t DecisionMemoKeyHasher::operator()(
    const DecisionMemoKey& key
) const noexcept {
    auto hash = kOrderDecisionFnvOffset;
    mix_u64(&hash, key.bundle_id);
    mix_u64(&hash, key.bundle_hash);
    mix_u64(&hash, key.snapshot_version_hash);
    mix_u64(&hash, key.policy_hash);
    mix_u64(&hash, key.source_intent_id);
    mix_u64(&hash, key.intent_expires_at_ns);
    mix_u64(&hash, key.now_ns);
    mix_u64(&hash, key.config_hash);
    return static_cast<std::size_t>(nonzero_hash(hash));
}

FixedShapeOrderDecisionSpec OrderDecisionCache::fixed_shape_spec(
    const signal::OpportunityIntent& intent,
    const oracle::CandidateBundle& bundle,
    bool* cache_hit
) {
    const auto key = spec_cache_key(intent, bundle);
    if (const auto existing = specs_.find(key); existing != specs_.end()) {
        if (cache_hit != nullptr) {
            *cache_hit = true;
        }
        return existing->second;
    }
    if (cache_hit != nullptr) {
        *cache_hit = false;
    }

    FixedShapeOrderDecisionSpec spec;
    spec.bundle_id = bundle.bundle_id;
    spec.oracle_artifact_hash = intent.oracle_artifact_hash;
    spec.bundle_hash = intent.bundle_hash;
    spec.constraint_hash = intent.constraint_hash;
    spec.leg_count = static_cast<std::uint8_t>(
        std::min<std::uint16_t>(bundle.leg_count, kMaxOrderDecisionLegs)
    );
    spec.guaranteed_payout_per_bundle_tick =
        bundle.guaranteed_payout_tick != 0
            ? bundle.guaranteed_payout_tick
            : intent.guaranteed_payout_tick;
    spec.min_unit_edge_tick = intent.min_edge_tick;
    spec.min_total_edge_tick = intent.min_edge_tick;
    spec.min_edge_bps = 0;
    spec.min_bundle_qty = intent.bundle_qty > 0 ? intent.bundle_qty : 1;

    for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
        const auto& bundle_leg = bundle.legs[i];
        spec.sides[i] = bundle_leg.side;
        spec.ratio_qty_lots[i] =
            bundle_leg.quantity_lots > 0 ? bundle_leg.quantity_lots : 1;
        if (intent.leg_count > i && intent.legs[i].quantity_lots > 0 &&
            bundle_leg.quantity_lots <= 0) {
            spec.ratio_qty_lots[i] = intent.legs[i].quantity_lots;
        }
        if (intent.leg_count > i) {
            spec.asset_indices[i] = intent.legs[i].asset_index;
            spec.market_indices[i] = 0;
        }
    }

    auto hash = kOrderDecisionFnvOffset;
    mix_u64(&hash, spec.bundle_id);
    mix_u64(&hash, spec.oracle_artifact_hash);
    mix_u64(&hash, spec.bundle_hash);
    mix_u64(&hash, spec.constraint_hash);
    mix_u64(&hash, spec.leg_count);
    mix_i64(&hash, spec.guaranteed_payout_per_bundle_tick);
    mix_i64(&hash, spec.min_unit_edge_tick);
    mix_i64(&hash, spec.min_total_edge_tick);
    mix_i64(&hash, spec.min_edge_bps);
    mix_i64(&hash, spec.min_bundle_qty);
    for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
        mix_u64(&hash, spec.asset_indices[i]);
        mix_u64(&hash, spec.market_indices[i]);
        mix_u64(&hash, side_value(spec.sides[i]));
        mix_i64(&hash, spec.ratio_qty_lots[i]);
    }
    spec.spec_hash = nonzero_hash(hash);

    specs_.emplace(key, spec);
    return spec;
}

const CandidateSet* OrderDecisionCache::find_candidate_set(
    const CandidateSetKey& key
) const {
    const auto found = candidate_sets_.find(key);
    return found == candidate_sets_.end() ? nullptr : &found->second;
}

const CandidateSet& OrderDecisionCache::store_candidate_set(
    const CandidateSetKey& key,
    std::span<const std::int64_t> q_values
) {
    CandidateSet set;
    set.count = static_cast<std::uint16_t>(
        std::min<std::size_t>(q_values.size(), set.q_values.size())
    );
    for (std::uint16_t i = 0; i < set.count; ++i) {
        set.q_values[i] = q_values[i];
    }
    set.candidate_set_hash = hash_candidate_set(
        key,
        std::span<const std::int64_t>{set.q_values.data(), set.count}
    );
    const auto [it, _] = candidate_sets_.insert_or_assign(key, set);
    return it->second;
}

std::optional<CostForQuantityResult> OrderDecisionCache::find_leg_cost(
    const LegCostKey& key
) const {
    const auto found = leg_costs_.find(key);
    if (found == leg_costs_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void OrderDecisionCache::store_leg_cost(
    const LegCostKey& key,
    const CostForQuantityResult& value
) {
    leg_costs_.insert_or_assign(key, value);
}

std::optional<OrderDecisionLite> OrderDecisionCache::find_decision(
    const DecisionMemoKey& key
) const {
    const auto found = decisions_.find(key);
    if (found == decisions_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void OrderDecisionCache::store_decision(
    const DecisionMemoKey& key,
    const OrderDecisionLite& decision
) {
    decisions_.insert_or_assign(key, decision);
}

void OrderDecisionCache::clear() {
    specs_.clear();
    candidate_sets_.clear();
    leg_costs_.clear();
    decisions_.clear();
}

std::uint64_t order_decision_config_hash(
    const OrderDecisionConfig& config
) noexcept {
    auto hash = kOrderDecisionFnvOffset;
    mix_u64(&hash, static_cast<std::uint64_t>(config.impl_mode));
    mix_u64(&hash, config.fast_fixed_shape_fallback_to_generic ? 1ULL : 0ULL);
    mix_i64(&hash, config.min_bundle_qty);
    mix_i64(&hash, config.max_bundle_qty);
    mix_i64(&hash, config.fee_per_bundle_tick);
    mix_i64(&hash, config.latency_buffer_per_bundle_tick);
    mix_i64(&hash, config.slippage_buffer_per_bundle_tick);
    mix_i64(&hash, config.price_protection_buffer_tick);
    mix_i64(&hash, config.max_allowed_price_tick);
    mix_u64(&hash, config.default_ttl_ns);
    mix_u64(&hash, config.use_prefix_vwap ? 1ULL : 0ULL);
    mix_u64(&hash, config.debug_compare_prefix_vs_linear ? 1ULL : 0ULL);
    return nonzero_hash(hash);
}

std::uint64_t depth_breakpoint_signature(
    std::span<const CostCurve> curves,
    const std::array<std::int64_t, kMaxOrderDecisionLegs>& ratio_qty_lots,
    const std::array<const state::MarketDepthView*, kMaxOrderDecisionLegs>&
        depth_views,
    std::uint16_t leg_count
) noexcept {
    auto hash = kOrderDecisionFnvOffset;
    mix_u64(&hash, leg_count);
    for (std::uint16_t i = 0; i < leg_count; ++i) {
        const auto* depth = depth_views[i];
        mix_u64(&hash, i);
        mix_i64(&hash, ratio_qty_lots[i]);
        if (depth != nullptr) {
            mix_u64(&hash, depth->asset_index);
            mix_u64(&hash, depth->ask_count);
            if (depth->prefix.ask_count > 0) {
                mix_u64(&hash, depth->prefix.ask_count);
                for (std::uint16_t level = 0;
                     level < depth->prefix.ask_count;
                     ++level) {
                    mix_i64(&hash, depth->prefix.ask_cum_qty[level]);
                }
            }
        }
        if (i < curves.size()) {
            const auto& curve = curves[i];
            mix_u64(&hash, curve.level_count);
            for (std::uint16_t level = 0; level < curve.level_count; ++level) {
                mix_i64(&hash, curve.levels[level].cumulative_qty_lots);
            }
        }
    }
    return nonzero_hash(hash);
}

std::uint64_t depth_price_signature(
    const state::MarketDepthView& depth
) noexcept {
    auto hash = kOrderDecisionFnvOffset;
    mix_u64(&hash, depth.asset_index);
    mix_u64(&hash, depth.book_version);
    mix_u64(&hash, depth.snapshot_version_hash);
    mix_u64(&hash, depth.ask_count);
    const auto count = std::min<std::uint16_t>(
        depth.ask_count,
        state::kMaxSnapshotDepth
    );
    for (std::uint16_t i = 0; i < count; ++i) {
        mix_i64(&hash, depth.asks[i].price_tick);
        mix_i64(
            &hash,
            common::math::price_level_size_lots(depth.asks[i])
        );
    }
    return nonzero_hash(hash);
}

CandidateSetKey make_candidate_set_key(
    const signal::OpportunityIntent& intent,
    const oracle::CandidateBundle& bundle,
    std::uint64_t breakpoint_signature,
    const OrderDecisionConfig& config
) noexcept {
    CandidateSetKey key;
    key.bundle_id = bundle.bundle_id;
    key.bundle_hash = intent.bundle_hash;
    key.depth_breakpoint_signature = breakpoint_signature;

    auto config_hash = kOrderDecisionFnvOffset;
    mix_i64(&config_hash, config.min_bundle_qty);
    mix_i64(&config_hash, config.max_bundle_qty);
    key.config_hash = nonzero_hash(config_hash);
    return key;
}

DecisionMemoKey make_decision_memo_key(
    const signal::OpportunityIntent& intent,
    const oracle::CandidateBundle& bundle,
    const risk::RiskPolicySnapshot& policy,
    const OrderDecisionConfig& config,
    std::uint64_t now_ns
) noexcept {
    DecisionMemoKey key;
    key.bundle_id = bundle.bundle_id;
    key.bundle_hash = intent.bundle_hash;
    key.snapshot_version_hash = intent.snapshot_version_hash;
    key.policy_hash =
        policy.policy_hash != 0 ? policy.policy_hash : risk::compute_policy_hash(policy);
    key.source_intent_id = intent.intent_id;
    key.intent_expires_at_ns = intent.expires_at_ns;
    key.now_ns = now_ns;
    key.config_hash = order_decision_config_hash(config);
    return key;
}

}  // namespace trading_engine::order_decision
