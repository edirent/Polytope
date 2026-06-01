#pragma once

#include <cstdint>

namespace trading_engine::order_decision {

enum class OrderDecisionImplMode : std::uint8_t {
    Generic,
    FastFixedShape
};

struct OrderDecisionConfig {
    OrderDecisionImplMode impl_mode = OrderDecisionImplMode::Generic;
    bool fast_fixed_shape_fallback_to_generic = true;

    std::int64_t min_bundle_qty = 1;
    std::int64_t max_bundle_qty = 0;

    std::int64_t fee_per_bundle_tick = 0;
    std::int64_t latency_buffer_per_bundle_tick = 0;
    std::int64_t slippage_buffer_per_bundle_tick = 0;

    std::int64_t price_protection_buffer_tick = 0;
    std::int64_t max_allowed_price_tick = 0;

    std::uint64_t default_ttl_ns = 1'000'000'000ULL;

    bool use_prefix_vwap = true;
    bool debug_compare_prefix_vs_linear = false;
};

}  // namespace trading_engine::order_decision
