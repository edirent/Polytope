#pragma once

#include <cstdint>

namespace trading_engine::order_decision {

struct PriceProtection {
    std::int64_t protection_buffer_tick = 0;
    std::int64_t max_allowed_price_tick = 0;
};

}  // namespace trading_engine::order_decision
