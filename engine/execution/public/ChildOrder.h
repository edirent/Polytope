#pragma once

#include "engine/execution/public/ExecutionTypes.h"

#include <cstdint>
#include <string>

namespace trading_engine::execution {

struct ChildOrder {
    ChildOrderId order_id = 0;
    PlanId plan_id = 0;

    std::string client_order_id;

    std::string market_id;
    std::string asset_id;

    OrderSide side = OrderSide::Buy;

    std::int64_t quantity_lots = 0;
    std::int64_t limit_price_tick = 0;

    std::int64_t estimated_vwap_tick = 0;
    std::int64_t worst_allowed_price_tick = 0;

    ChildOrderStatus status = ChildOrderStatus::Created;
};

}  // namespace trading_engine::execution
