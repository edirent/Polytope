#pragma once

#include "engine/execution/public/ExecutionTypes.h"

#include <cstdint>
#include <string>

namespace trading_engine::execution {

struct ExecutionReport {
    PlanId plan_id = 0;
    ChildOrderId child_order_id = 0;

    ChildOrderStatus status = ChildOrderStatus::Created;

    std::string venue_order_id;
    std::string reject_reason;

    std::int64_t filled_lots = 0;
    std::int64_t remaining_lots = 0;
    std::int64_t avg_fill_price_tick = 0;

    std::uint64_t event_ts_ns = 0;
};

}  // namespace trading_engine::execution
