#include "engine/signal/metrics/SignalMetrics.h"

#include <algorithm>

namespace trading_engine::signal {

void SignalMetrics::observe_scan_latency(
    std::uint64_t latency_ns
) noexcept {
    ++scan_latency_ns.count;
    scan_latency_ns.last_ns = latency_ns;
    scan_latency_ns.total_ns += latency_ns;
    if (scan_latency_ns.count == 1) {
        scan_latency_ns.min_ns = latency_ns;
        scan_latency_ns.max_ns = latency_ns;
        return;
    }
    scan_latency_ns.min_ns = std::min(scan_latency_ns.min_ns, latency_ns);
    scan_latency_ns.max_ns = std::max(scan_latency_ns.max_ns, latency_ns);
}

}  // namespace trading_engine::signal
