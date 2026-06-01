#pragma once

#include "engine/order_decision/math/CostCurve.h"
#include "engine/order_decision/public/OrderDecisionEvalStats.h"
#include "engine/state/view/MarketDepthView.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace trading_engine::order_decision {

struct SizeCandidateEnumerationInput {
    std::span<const CostCurve> curves;
    std::array<std::int64_t, kMaxOrderDecisionLegs> ratio_qty_lots{};
    std::array<
        const trading_engine::state::MarketDepthView*,
        kMaxOrderDecisionLegs
    > depth_views{};
    std::uint16_t leg_count = 0;

    std::int64_t min_bundle_qty = 1;
    std::int64_t max_bundle_qty = 0;
    std::int64_t max_total_cost_tick = 0;

    bool use_prefix_vwap = true;
    bool debug_compare_prefix_vs_linear = false;
};

struct SizeCandidateEnumerationResult {
    std::vector<std::int64_t> candidates;

    std::uint64_t candidate_enumerate_ns = 0;
    std::uint64_t candidate_sort_dedup_ns = 0;

    OrderDecisionEvalStats eval_stats;
};

class KWayCandidateCursor {
public:
    explicit KWayCandidateCursor(
        const SizeCandidateEnumerationInput& input
    ) noexcept;

    [[nodiscard]] bool next(
        std::int64_t* bundle_qty,
        OrderDecisionEvalStats* eval_stats = nullptr
    ) noexcept;

private:
    const SizeCandidateEnumerationInput* input_ = nullptr;
    std::array<std::uint16_t, kMaxOrderDecisionLegs> levels_{};
    std::int64_t last_emitted_ = 0;
    bool done_ = false;
};

class SizeCandidateEnumerator {
public:
    [[nodiscard]] SizeCandidateEnumerationResult enumerate(
        const SizeCandidateEnumerationInput& input
    ) const;
};

}  // namespace trading_engine::order_decision
