#pragma once

#include <cstdint>

namespace trading_engine::order_decision {

struct OrderDecisionStageTimings {
    std::uint64_t total_ns = 0;

    std::uint64_t input_validate_ns = 0;
    std::uint64_t depth_view_prepare_ns = 0;
    std::uint64_t cost_curve_build_ns = 0;
    std::uint64_t candidate_enumerate_ns = 0;
    std::uint64_t candidate_sort_dedup_ns = 0;
    std::uint64_t candidate_eval_ns = 0;
    std::uint64_t limit_price_build_ns = 0;
    std::uint64_t decision_build_ns = 0;
    std::uint64_t decision_hash_ns = 0;
    std::uint64_t debug_materialize_ns = 0;
    std::uint64_t publisher_ns = 0;

    std::uint64_t stage_sum_ns = 0;
    std::uint64_t unattributed_ns = 0;
};

[[nodiscard]] inline std::uint64_t stage_sum(
    const OrderDecisionStageTimings& timings
) noexcept {
    return timings.input_validate_ns + timings.depth_view_prepare_ns +
           timings.cost_curve_build_ns + timings.candidate_enumerate_ns +
           timings.candidate_sort_dedup_ns + timings.candidate_eval_ns +
           timings.limit_price_build_ns + timings.decision_build_ns +
           timings.decision_hash_ns + timings.debug_materialize_ns +
           timings.publisher_ns;
}

inline void finalize_stage_timings(
    OrderDecisionStageTimings* timings
) noexcept {
    if (timings == nullptr) {
        return;
    }
    timings->stage_sum_ns = stage_sum(*timings);
    timings->unattributed_ns =
        timings->total_ns > timings->stage_sum_ns
            ? timings->total_ns - timings->stage_sum_ns
            : 0;
}

inline void add_stage_timings(
    OrderDecisionStageTimings* lhs,
    const OrderDecisionStageTimings& rhs
) noexcept {
    if (lhs == nullptr) {
        return;
    }
    lhs->input_validate_ns += rhs.input_validate_ns;
    lhs->depth_view_prepare_ns += rhs.depth_view_prepare_ns;
    lhs->cost_curve_build_ns += rhs.cost_curve_build_ns;
    lhs->candidate_enumerate_ns += rhs.candidate_enumerate_ns;
    lhs->candidate_sort_dedup_ns += rhs.candidate_sort_dedup_ns;
    lhs->candidate_eval_ns += rhs.candidate_eval_ns;
    lhs->limit_price_build_ns += rhs.limit_price_build_ns;
    lhs->decision_build_ns += rhs.decision_build_ns;
    lhs->decision_hash_ns += rhs.decision_hash_ns;
    lhs->debug_materialize_ns += rhs.debug_materialize_ns;
    lhs->publisher_ns += rhs.publisher_ns;
}

}  // namespace trading_engine::order_decision
