#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::paper {

struct PaperAccount {
    std::string account_id;

    std::int64_t starting_cash_tick = 0;
    std::int64_t cash_balance_tick = 0;
    std::int64_t reserved_cash_tick = 0;
    std::int64_t realized_pnl_tick = 0;
    std::int64_t unrealized_pnl_tick = 0;

    std::uint64_t version = 0;
    std::uint64_t updated_ts_ns = 0;
};

}  // namespace trading_engine::paper
