#include "engine/order_decision/sizing/SizeCandidateEnumerator.h"

#include "engine/order_decision/math/PrefixVwap.h"

#include <chrono>
#include <limits>

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

[[nodiscard]] bool has_ask_prefix(
    const trading_engine::state::MarketDepthView* depth
) noexcept {
    return depth != nullptr && depth->prefix.ask_count > 0 &&
           depth->prefix.ask_count <= depth->ask_count &&
           depth->prefix.ask_cum_qty[depth->prefix.ask_count - 1U] > 0;
}

[[nodiscard]] CostForQuantityResult cost_for_quantity_recorded(
    const CostCurve& curve,
    const trading_engine::state::MarketDepthView* depth,
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

[[nodiscard]] bool candidate_within_budget(
    const SizeCandidateEnumerationInput& input,
    std::int64_t bundle_qty,
    OrderDecisionEvalStats* eval_stats
) noexcept {
    if (input.max_total_cost_tick <= 0) {
        return true;
    }

    std::int64_t total_cost = 0;
    for (std::uint16_t i = 0; i < input.leg_count; ++i) {
        const auto leg_qty = input.ratio_qty_lots[i] * bundle_qty;
        const auto priced = cost_for_quantity_recorded(
            input.curves[i],
            input.depth_views[i],
            leg_qty,
            input.use_prefix_vwap,
            input.debug_compare_prefix_vs_linear,
            eval_stats
        );
        if (!priced.ok) {
            return false;
        }
        total_cost += priced.total_cost_tick;
        if (total_cost > input.max_total_cost_tick) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::int64_t candidate_at(
    const CostCurve& curve,
    std::int64_t ratio,
    std::uint16_t level,
    std::int64_t max_bundle_qty
) noexcept {
    if (ratio <= 0 || level >= curve.level_count) {
        return 0;
    }
    auto q = curve.levels[level].cumulative_qty_lots / ratio;
    if (q <= 0) {
        return 0;
    }
    if (max_bundle_qty > 0 && q > max_bundle_qty) {
        q = max_bundle_qty;
    }
    return q;
}

void advance_invalid_candidates(
    const SizeCandidateEnumerationInput& input,
    std::array<std::uint16_t, kMaxOrderDecisionLegs>* levels
) noexcept {
    for (std::uint16_t leg = 0; leg < input.leg_count; ++leg) {
        const auto ratio = input.ratio_qty_lots[leg];
        const auto& curve = input.curves[leg];
        while ((*levels)[leg] < curve.level_count) {
            const auto q = candidate_at(
                curve,
                ratio,
                (*levels)[leg],
                input.max_bundle_qty
            );
            if (q >= input.min_bundle_qty) {
                break;
            }
            ++(*levels)[leg];
        }
    }
}

}  // namespace

KWayCandidateCursor::KWayCandidateCursor(
    const SizeCandidateEnumerationInput& input
) noexcept : input_(&input) {}

bool KWayCandidateCursor::next(
    std::int64_t* bundle_qty,
    OrderDecisionEvalStats* eval_stats
) noexcept {
    if (bundle_qty == nullptr || input_ == nullptr || done_ ||
        input_->leg_count == 0 || input_->curves.size() < input_->leg_count) {
        return false;
    }

    while (true) {
        advance_invalid_candidates(*input_, &levels_);

        auto next_q = std::numeric_limits<std::int64_t>::max();
        for (std::uint16_t leg = 0; leg < input_->leg_count; ++leg) {
            const auto q = candidate_at(
                input_->curves[leg],
                input_->ratio_qty_lots[leg],
                levels_[leg],
                input_->max_bundle_qty
            );
            if (q >= input_->min_bundle_qty && q < next_q) {
                next_q = q;
            }
        }
        if (next_q == std::numeric_limits<std::int64_t>::max()) {
            done_ = true;
            return false;
        }

        for (std::uint16_t leg = 0; leg < input_->leg_count; ++leg) {
            while (levels_[leg] < input_->curves[leg].level_count) {
                const auto q = candidate_at(
                    input_->curves[leg],
                    input_->ratio_qty_lots[leg],
                    levels_[leg],
                    input_->max_bundle_qty
                );
                if (q > next_q) {
                    break;
                }
                ++levels_[leg];
            }
        }

        if (next_q == last_emitted_) {
            continue;
        }
        if (!candidate_within_budget(*input_, next_q, eval_stats)) {
            continue;
        }

        last_emitted_ = next_q;
        *bundle_qty = next_q;
        return true;
    }
}

SizeCandidateEnumerationResult SizeCandidateEnumerator::enumerate(
    const SizeCandidateEnumerationInput& input
) const {
    SizeCandidateEnumerationResult result;
    const auto enumerate_start = Clock::now();
    if (input.leg_count == 0 || input.curves.size() < input.leg_count) {
        result.candidate_enumerate_ns = elapsed_ns(enumerate_start);
        return result;
    }

    result.candidates.reserve(
        static_cast<std::size_t>(input.leg_count) * kMaxCostCurveLevels
    );

    KWayCandidateCursor cursor(input);
    std::int64_t q = 0;
    while (cursor.next(&q, &result.eval_stats)) {
        result.candidates.push_back(q);
    }
    result.candidate_enumerate_ns = elapsed_ns(enumerate_start);
    result.eval_stats.candidate_count =
        static_cast<std::uint32_t>(result.candidates.size());
    result.candidate_sort_dedup_ns = 0;
    return result;
}

}  // namespace trading_engine::order_decision
