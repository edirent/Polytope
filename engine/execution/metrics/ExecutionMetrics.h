#pragma once

#include <cstdint>

namespace trading_engine::execution {

struct ExecutionLatencyMetric {
    std::uint64_t count = 0;
    std::uint64_t last_ns = 0;
    std::uint64_t min_ns = 0;
    std::uint64_t max_ns = 0;
    std::uint64_t total_ns = 0;
};

struct ExecutionMetrics {
    std::uint64_t plan_created = 0;
    std::uint64_t plan_submitted = 0;
    std::uint64_t plan_filled = 0;
    std::uint64_t plan_failed = 0;
    std::uint64_t plan_expired = 0;
    std::uint64_t plan_hedge_required = 0;

    std::uint64_t child_created = 0;
    std::uint64_t child_filled = 0;
    std::uint64_t child_partial = 0;
    std::uint64_t child_cancelled = 0;
    std::uint64_t child_failed = 0;

    std::uint64_t report_published = 0;

    std::uint64_t reservation_consume = 0;
    std::uint64_t reservation_release = 0;
    std::uint64_t reservation_expire = 0;
    std::uint64_t reservation_hedge_required = 0;

    ExecutionLatencyMetric submit_latency_ns;
    ExecutionLatencyMetric fill_simulation_latency_ns;

    void observe_submit_latency(std::uint64_t latency_ns) noexcept;
    void observe_fill_simulation_latency(std::uint64_t latency_ns) noexcept;
};

}  // namespace trading_engine::execution
