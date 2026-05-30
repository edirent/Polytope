#pragma once

#include <cstdint>

namespace trading_engine::signal {

struct SignalLatencyMetric {
    std::uint64_t count = 0;
    std::uint64_t last_ns = 0;
    std::uint64_t min_ns = 0;
    std::uint64_t max_ns = 0;
    std::uint64_t total_ns = 0;
};

struct SignalMetrics {
    std::uint64_t scan_count = 0;
    std::uint64_t bundle_scanned = 0;
    std::uint64_t bundle_rejected = 0;
    std::uint64_t bundle_passed = 0;

    std::uint64_t reject_settled = 0;
    std::uint64_t reject_missing_snapshot = 0;
    std::uint64_t reject_stale_lob = 0;
    std::uint64_t reject_snapshot_skew = 0;
    std::uint64_t reject_insufficient_depth = 0;
    std::uint64_t reject_edge_below_threshold = 0;
    std::uint64_t reject_duplicate = 0;
    std::uint64_t reject_rate_limited = 0;

    std::uint64_t intent_published = 0;

    SignalLatencyMetric scan_latency_ns;

    void observe_scan_latency(std::uint64_t latency_ns) noexcept;
};

}  // namespace trading_engine::signal
