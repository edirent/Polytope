#pragma once

#include "engine/signal/reader/SnapshotVersion.h"

#include <cstdint>

namespace trading_engine::signal {

struct SignalConfig {
    bool require_usable_for_depth = true;
    bool require_usable_for_signal = false;

    std::int64_t default_fee_tick = 0;
    std::int64_t default_latency_buffer_tick = 0;
    std::int64_t slippage_buffer_tick = 0;

    std::int64_t min_unit_edge_tick = 0;
    std::int64_t min_total_edge_tick = 0;
    std::int64_t min_edge_bps = 0;
    std::int64_t min_bundle_qty = 1;

    std::uint32_t max_intents_per_scan = 1024;
    std::int32_t max_intents_per_second = 100;
    std::uint32_t max_bundle_legs = 16;
    std::uint64_t intent_ttl_ns = 5'000'000'000ULL;

    bool emit_rejections = true;

    std::int64_t max_lob_age_ns = 1'000'000'000;
    std::uint64_t max_snapshot_version_skew = 10;
    SnapshotConsistencyMode consistency_mode =
        SnapshotConsistencyMode::BoundedSkew;
};

}  // namespace trading_engine::signal
