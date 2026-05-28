#pragma once

#include <cstdint>

namespace trading_engine::signal {

struct SignalConfig {
    bool require_usable_for_depth = true;
    bool require_usable_for_signal = false;

    std::int64_t default_fee_tick = 0;
    std::int64_t default_latency_buffer_tick = 0;

    std::uint32_t max_intents_per_scan = 1024;
    std::uint32_t max_bundle_legs = 16;

    bool emit_rejections = true;
};

}  // namespace trading_engine::signal
