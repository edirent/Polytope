#pragma once

#include <cstdint>

namespace trading_engine::paper {

struct CashLedger {
    std::int64_t starting_cash_tick = 0;
    std::int64_t cash_tick = 0;
    std::int64_t reserved_cash_tick = 0;
    std::int64_t realized_pnl_tick = 0;
    std::int64_t fees_paid_tick = 0;
};

}  // namespace trading_engine::paper
