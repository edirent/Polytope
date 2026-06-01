#pragma once

#include <cstdint>

namespace trading_engine::order_decision {

struct OrderDecisionEvalStats {
    std::uint32_t candidate_count = 0;
    std::uint32_t candidates_evaluated = 0;

    std::uint32_t vwap_cost_calls = 0;
    std::uint32_t depth_levels_scanned = 0;
    std::uint32_t max_depth_levels_scanned = 0;

    std::uint32_t linear_cost_calls = 0;
    std::uint32_t prefix_cost_calls = 0;
    std::uint32_t prefix_linear_mismatch = 0;

    std::uint32_t fixed_shape_spec_cache_hits = 0;
    std::uint32_t fixed_shape_spec_cache_misses = 0;
    std::uint32_t candidate_set_cache_hits = 0;
    std::uint32_t candidate_set_cache_misses = 0;
    std::uint32_t leg_cost_cache_hits = 0;
    std::uint32_t leg_cost_cache_misses = 0;
    std::uint32_t decision_memo_cache_hits = 0;
    std::uint32_t decision_memo_cache_misses = 0;

    std::uint32_t incremental_eval_steps = 0;
    std::uint32_t incremental_leg_updates = 0;
    std::uint32_t incremental_level_advances = 0;

    std::uint32_t rejected_by_unit_edge = 0;
    std::uint32_t rejected_by_total_edge = 0;
    std::uint32_t rejected_by_bps = 0;
};

inline void add_eval_stats(
    OrderDecisionEvalStats* lhs,
    const OrderDecisionEvalStats& rhs
) noexcept {
    if (lhs == nullptr) {
        return;
    }
    lhs->candidate_count += rhs.candidate_count;
    lhs->candidates_evaluated += rhs.candidates_evaluated;
    lhs->vwap_cost_calls += rhs.vwap_cost_calls;
    lhs->depth_levels_scanned += rhs.depth_levels_scanned;
    if (rhs.max_depth_levels_scanned > lhs->max_depth_levels_scanned) {
        lhs->max_depth_levels_scanned = rhs.max_depth_levels_scanned;
    }
    lhs->linear_cost_calls += rhs.linear_cost_calls;
    lhs->prefix_cost_calls += rhs.prefix_cost_calls;
    lhs->prefix_linear_mismatch += rhs.prefix_linear_mismatch;
    lhs->fixed_shape_spec_cache_hits += rhs.fixed_shape_spec_cache_hits;
    lhs->fixed_shape_spec_cache_misses += rhs.fixed_shape_spec_cache_misses;
    lhs->candidate_set_cache_hits += rhs.candidate_set_cache_hits;
    lhs->candidate_set_cache_misses += rhs.candidate_set_cache_misses;
    lhs->leg_cost_cache_hits += rhs.leg_cost_cache_hits;
    lhs->leg_cost_cache_misses += rhs.leg_cost_cache_misses;
    lhs->decision_memo_cache_hits += rhs.decision_memo_cache_hits;
    lhs->decision_memo_cache_misses += rhs.decision_memo_cache_misses;
    lhs->incremental_eval_steps += rhs.incremental_eval_steps;
    lhs->incremental_leg_updates += rhs.incremental_leg_updates;
    lhs->incremental_level_advances += rhs.incremental_level_advances;
    lhs->rejected_by_unit_edge += rhs.rejected_by_unit_edge;
    lhs->rejected_by_total_edge += rhs.rejected_by_total_edge;
    lhs->rejected_by_bps += rhs.rejected_by_bps;
}

}  // namespace trading_engine::order_decision
