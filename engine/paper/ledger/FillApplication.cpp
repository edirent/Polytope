#include "engine/paper/ledger/FillApplication.h"

namespace trading_engine::paper {

namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) {
        *hash ^= static_cast<std::uint8_t>((value >> (i * 8)) & 0xffU);
        *hash *= kFnvPrime;
    }
}

}  // namespace

std::uint64_t derive_execution_report_id(const FillApplication& fill) noexcept {
    if (fill.execution_report_id != 0) {
        return fill.execution_report_id;
    }

    std::uint64_t hash = kFnvOffset;
    mix(&hash, fill.report.plan_id);
    mix(&hash, fill.report.child_order_id);
    mix(&hash, fill.report.event_ts_ns);
    mix(&hash, static_cast<std::uint64_t>(fill.report.status));
    mix(&hash, static_cast<std::uint64_t>(fill.report.filled_lots));
    mix(&hash, static_cast<std::uint64_t>(fill.report.avg_fill_price_tick));
    return hash;
}

}  // namespace trading_engine::paper
