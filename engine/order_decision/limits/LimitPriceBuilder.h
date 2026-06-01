#pragma once

#include "engine/order_decision/public/OrderDecisionTypes.h"

#include <cstdint>

namespace trading_engine::order_decision {

struct LimitPriceInput {
    std::int64_t worst_consumed_price_tick = 0;
    std::int64_t protection_buffer_tick = 0;
    std::int64_t leg_max_price_tick = 0;
    std::int64_t configured_max_price_tick = 0;
};

struct LimitPriceResult {
    bool ok = false;
    std::int64_t limit_price_tick = 0;
    OrderDecisionType reject_reason = OrderDecisionType::NoTrade;
};

class LimitPriceBuilder {
public:
    [[nodiscard]] LimitPriceResult build_buy_limit(
        const LimitPriceInput& input
    ) const;
};

}  // namespace trading_engine::order_decision
