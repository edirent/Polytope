#pragma once

#include <cstdint>

namespace trading_engine::risk {

struct RiskLatencyMetric {
    std::uint64_t count = 0;
    std::uint64_t last_ns = 0;
    std::uint64_t min_ns = 0;
    std::uint64_t max_ns = 0;
    std::uint64_t total_ns = 0;
};

struct RiskMetrics {
    std::uint64_t evaluate_count = 0;
    std::uint64_t approve_count = 0;
    std::uint64_t reject_count = 0;

    std::uint64_t reject_invalid_intent = 0;
    std::uint64_t reject_expired = 0;
    std::uint64_t reject_duplicate = 0;
    std::uint64_t reject_kill_switch = 0;
    std::uint64_t reject_stale_book = 0;
    std::uint64_t reject_insufficient_depth = 0;
    std::uint64_t reject_cost_drift = 0;
    std::uint64_t reject_low_edge = 0;
    std::uint64_t reject_exposure = 0;
    std::uint64_t reject_inventory = 0;
    std::uint64_t reject_partial_fill = 0;
    std::uint64_t reject_max_loss = 0;
    std::uint64_t reject_rate_limited = 0;

    std::uint64_t reservation_created = 0;
    std::uint64_t reservation_expired = 0;
    std::uint64_t reservation_released = 0;

    std::uint64_t vwap_reused_signal_cost = 0;
    std::uint64_t vwap_reused_signal_snapshot = 0;
    std::uint64_t vwap_recomputed = 0;
    std::uint64_t snapshot_fast_path = 0;
    std::uint64_t snapshot_requery = 0;
    RiskLatencyMetric evaluate_latency_ns;

    void observe_evaluate_latency(std::uint64_t latency_ns) noexcept;
};

}  // namespace trading_engine::risk
