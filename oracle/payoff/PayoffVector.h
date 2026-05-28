#pragma once

#include <cstdint>

namespace trading_engine::oracle {

struct PayoffEntry {
    std::uint64_t state_id = 0;
    std::uint32_t asset_index = 0;
    std::int64_t payout_tick = 0;
};

}  // namespace trading_engine::oracle
