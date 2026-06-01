#pragma once

#include "engine/order_decision/math/CostCurve.h"
#include "engine/order_decision/public/OrderDecision.h"
#include "engine/order_decision/public/OrderDecisionConfig.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/view/MarketDepthView.h"
#include "oracle/public/CandidateBundle.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>

namespace trading_engine::order_decision {

inline constexpr std::uint16_t kMaxOrderDecisionCandidates =
    kMaxOrderDecisionLegs * kMaxCostCurveLevels;

struct FixedShapeOrderDecisionSpec {
    std::uint64_t spec_hash = 0;

    std::uint64_t bundle_id = 0;
    std::uint64_t oracle_artifact_hash = 0;
    std::uint64_t bundle_hash = 0;
    std::uint64_t constraint_hash = 0;

    std::uint8_t leg_count = 0;

    std::array<std::uint32_t, kMaxOrderDecisionLegs> asset_indices{};
    std::array<std::uint32_t, kMaxOrderDecisionLegs> market_indices{};

    std::array<Side, kMaxOrderDecisionLegs> sides{};
    std::array<std::int64_t, kMaxOrderDecisionLegs> ratio_qty_lots{};

    std::int64_t guaranteed_payout_per_bundle_tick = 0;

    std::int64_t min_unit_edge_tick = 0;
    std::int64_t min_total_edge_tick = 0;
    std::int64_t min_edge_bps = 0;
    std::int64_t min_bundle_qty = 0;
};

struct CandidateSetKey {
    std::uint64_t bundle_id = 0;
    std::uint64_t bundle_hash = 0;
    std::uint64_t depth_breakpoint_signature = 0;
    std::uint64_t config_hash = 0;

    [[nodiscard]] bool operator==(const CandidateSetKey& other) const noexcept {
        return bundle_id == other.bundle_id &&
               bundle_hash == other.bundle_hash &&
               depth_breakpoint_signature ==
                   other.depth_breakpoint_signature &&
               config_hash == other.config_hash;
    }
};

struct CandidateSetKeyHasher {
    [[nodiscard]] std::size_t operator()(
        const CandidateSetKey& key
    ) const noexcept;
};

struct CandidateSet {
    std::uint64_t candidate_set_hash = 0;
    std::uint16_t count = 0;
    std::array<std::int64_t, kMaxOrderDecisionCandidates> q_values{};
};

struct LegCostKey {
    std::uint64_t candidate_set_hash = 0;
    std::uint8_t leg_index = 0;
    std::uint32_t asset_index = 0;
    std::uint64_t book_version = 0;
    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t depth_price_signature = 0;
    std::int64_t q = 0;
    std::int64_t ratio_qty_lots = 0;
    bool use_prefix_vwap = true;

    [[nodiscard]] bool operator==(const LegCostKey& other) const noexcept {
        return candidate_set_hash == other.candidate_set_hash &&
               leg_index == other.leg_index &&
               asset_index == other.asset_index &&
               book_version == other.book_version &&
               snapshot_version_hash == other.snapshot_version_hash &&
               depth_price_signature == other.depth_price_signature &&
               q == other.q && ratio_qty_lots == other.ratio_qty_lots &&
               use_prefix_vwap == other.use_prefix_vwap;
    }
};

struct LegCostKeyHasher {
    [[nodiscard]] std::size_t operator()(const LegCostKey& key) const noexcept;
};

struct DecisionMemoKey {
    std::uint64_t bundle_id = 0;
    std::uint64_t bundle_hash = 0;
    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t policy_hash = 0;
    std::uint64_t source_intent_id = 0;
    std::uint64_t intent_expires_at_ns = 0;
    std::uint64_t now_ns = 0;
    std::uint64_t config_hash = 0;

    [[nodiscard]] bool operator==(const DecisionMemoKey& other) const noexcept {
        return bundle_id == other.bundle_id &&
               bundle_hash == other.bundle_hash &&
               snapshot_version_hash == other.snapshot_version_hash &&
               policy_hash == other.policy_hash &&
               source_intent_id == other.source_intent_id &&
               intent_expires_at_ns == other.intent_expires_at_ns &&
               now_ns == other.now_ns && config_hash == other.config_hash;
    }
};

struct DecisionMemoKeyHasher {
    [[nodiscard]] std::size_t operator()(
        const DecisionMemoKey& key
    ) const noexcept;
};

class OrderDecisionCache {
public:
    [[nodiscard]] FixedShapeOrderDecisionSpec fixed_shape_spec(
        const signal::OpportunityIntent& intent,
        const oracle::CandidateBundle& bundle,
        bool* cache_hit = nullptr
    );

    [[nodiscard]] const CandidateSet* find_candidate_set(
        const CandidateSetKey& key
    ) const;
    [[nodiscard]] const CandidateSet& store_candidate_set(
        const CandidateSetKey& key,
        std::span<const std::int64_t> q_values
    );

    [[nodiscard]] std::optional<CostForQuantityResult> find_leg_cost(
        const LegCostKey& key
    ) const;
    void store_leg_cost(
        const LegCostKey& key,
        const CostForQuantityResult& value
    );

    [[nodiscard]] std::optional<OrderDecisionLite> find_decision(
        const DecisionMemoKey& key
    ) const;
    void store_decision(
        const DecisionMemoKey& key,
        const OrderDecisionLite& decision
    );

    void clear();

private:
    std::unordered_map<std::uint64_t, FixedShapeOrderDecisionSpec> specs_;
    std::unordered_map<CandidateSetKey, CandidateSet, CandidateSetKeyHasher>
        candidate_sets_;
    std::unordered_map<LegCostKey, CostForQuantityResult, LegCostKeyHasher>
        leg_costs_;
    std::unordered_map<DecisionMemoKey, OrderDecisionLite, DecisionMemoKeyHasher>
        decisions_;
};

[[nodiscard]] std::uint64_t order_decision_config_hash(
    const OrderDecisionConfig& config
) noexcept;

[[nodiscard]] std::uint64_t depth_breakpoint_signature(
    std::span<const CostCurve> curves,
    const std::array<std::int64_t, kMaxOrderDecisionLegs>& ratio_qty_lots,
    const std::array<const state::MarketDepthView*, kMaxOrderDecisionLegs>&
        depth_views,
    std::uint16_t leg_count
) noexcept;

[[nodiscard]] std::uint64_t depth_price_signature(
    const state::MarketDepthView& depth
) noexcept;

[[nodiscard]] CandidateSetKey make_candidate_set_key(
    const signal::OpportunityIntent& intent,
    const oracle::CandidateBundle& bundle,
    std::uint64_t breakpoint_signature,
    const OrderDecisionConfig& config
) noexcept;

[[nodiscard]] DecisionMemoKey make_decision_memo_key(
    const signal::OpportunityIntent& intent,
    const oracle::CandidateBundle& bundle,
    const risk::RiskPolicySnapshot& policy,
    const OrderDecisionConfig& config,
    std::uint64_t now_ns
) noexcept;

}  // namespace trading_engine::order_decision
