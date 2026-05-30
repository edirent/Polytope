#include "engine/risk/metrics/RiskMetrics.h"

#include <algorithm>

namespace trading_engine::risk {

void RiskMetrics::observe_evaluate_latency(
    std::uint64_t latency_ns
) noexcept {
    ++evaluate_latency_ns.count;
    evaluate_latency_ns.last_ns = latency_ns;
    evaluate_latency_ns.total_ns += latency_ns;
    if (evaluate_latency_ns.count == 1) {
        evaluate_latency_ns.min_ns = latency_ns;
        evaluate_latency_ns.max_ns = latency_ns;
        return;
    }
    evaluate_latency_ns.min_ns =
        std::min(evaluate_latency_ns.min_ns, latency_ns);
    evaluate_latency_ns.max_ns =
        std::max(evaluate_latency_ns.max_ns, latency_ns);
}

}  // namespace trading_engine::risk
