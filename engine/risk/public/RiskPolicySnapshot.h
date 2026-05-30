#pragma once

#include <bit>
#include <cstdint>

namespace trading_engine::risk {

struct RiskPolicySnapshot {
    std::uint64_t policy_version = 1;
    std::uint64_t policy_hash = 0;

    bool risk_enabled = true;
    bool kill_switch_enabled = false;

    std::int64_t min_post_risk_total_edge_tick = 0;
    std::int64_t min_post_risk_unit_edge_tick = 0;
    std::int64_t min_edge_bps = 0;

    std::int64_t max_total_cost_tick = 0;
    std::int64_t max_single_market_exposure_tick = 0;
    std::int64_t max_total_exposure_tick = 0;
    std::int64_t max_inventory_lots_per_asset = 0;

    std::int64_t max_book_age_ns = 1'000'000'000;
    std::int64_t max_intent_age_ns = 1'000'000'000;
    std::int64_t max_snapshot_skew_ns = 0;

    std::int64_t max_allowed_cost_drift_tick = 0;
    std::int64_t max_slippage_tick = 0;
    std::int64_t max_unhedged_loss_tick = 0;

    double min_depth_margin_ratio = 1.20;

    std::uint32_t max_pending_intents_per_bundle = 1;
    std::uint32_t max_pending_intents_total = 1024;
    std::uint32_t max_approvals_per_second = 100;
};

namespace detail {

inline constexpr std::uint64_t kRiskFnvOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kRiskFnvPrime = 1099511628211ULL;

inline void mix_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= (value >> shift) & 0xffU;
        *hash *= kRiskFnvPrime;
    }
}

inline void mix_i64(std::uint64_t* hash, std::int64_t value) noexcept {
    mix_u64(hash, static_cast<std::uint64_t>(value));
}

inline void mix_bool(std::uint64_t* hash, bool value) noexcept {
    *hash ^= value ? 1U : 0U;
    *hash *= kRiskFnvPrime;
}

inline void mix_u32(std::uint64_t* hash, std::uint32_t value) noexcept {
    mix_u64(hash, value);
}

inline void mix_double(std::uint64_t* hash, double value) noexcept {
    mix_u64(hash, std::bit_cast<std::uint64_t>(value));
}

}  // namespace detail

[[nodiscard]] inline std::uint64_t compute_policy_hash(
    const RiskPolicySnapshot& policy
) noexcept {
    auto hash = detail::kRiskFnvOffset;
    detail::mix_u64(&hash, policy.policy_version);
    detail::mix_bool(&hash, policy.risk_enabled);
    detail::mix_bool(&hash, policy.kill_switch_enabled);
    detail::mix_i64(&hash, policy.min_post_risk_total_edge_tick);
    detail::mix_i64(&hash, policy.min_post_risk_unit_edge_tick);
    detail::mix_i64(&hash, policy.min_edge_bps);
    detail::mix_i64(&hash, policy.max_total_cost_tick);
    detail::mix_i64(&hash, policy.max_single_market_exposure_tick);
    detail::mix_i64(&hash, policy.max_total_exposure_tick);
    detail::mix_i64(&hash, policy.max_inventory_lots_per_asset);
    detail::mix_i64(&hash, policy.max_book_age_ns);
    detail::mix_i64(&hash, policy.max_intent_age_ns);
    detail::mix_i64(&hash, policy.max_snapshot_skew_ns);
    detail::mix_i64(&hash, policy.max_allowed_cost_drift_tick);
    detail::mix_i64(&hash, policy.max_slippage_tick);
    detail::mix_i64(&hash, policy.max_unhedged_loss_tick);
    detail::mix_double(&hash, policy.min_depth_margin_ratio);
    detail::mix_u32(&hash, policy.max_pending_intents_per_bundle);
    detail::mix_u32(&hash, policy.max_pending_intents_total);
    detail::mix_u32(&hash, policy.max_approvals_per_second);
    return hash;
}

[[nodiscard]] inline RiskPolicySnapshot with_computed_policy_hash(
    RiskPolicySnapshot policy
) noexcept {
    policy.policy_hash = compute_policy_hash(policy);
    return policy;
}

}  // namespace trading_engine::risk
