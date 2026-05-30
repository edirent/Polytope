#include "engine/execution/metrics/ExecutionMetrics.h"

#include <algorithm>

namespace trading_engine::execution {

namespace {

void observe_latency(
    ExecutionLatencyMetric* metric,
    std::uint64_t latency_ns
) noexcept {
    ++metric->count;
    metric->last_ns = latency_ns;
    metric->total_ns += latency_ns;
    if (metric->count == 1) {
        metric->min_ns = latency_ns;
        metric->max_ns = latency_ns;
        return;
    }
    metric->min_ns = std::min(metric->min_ns, latency_ns);
    metric->max_ns = std::max(metric->max_ns, latency_ns);
}

}  // namespace

void ExecutionMetrics::observe_submit_latency(
    std::uint64_t latency_ns
) noexcept {
    observe_latency(&submit_latency_ns, latency_ns);
}

void ExecutionMetrics::observe_fill_simulation_latency(
    std::uint64_t latency_ns
) noexcept {
    observe_latency(&fill_simulation_latency_ns, latency_ns);
}

}  // namespace trading_engine::execution
