#pragma once

#include "oracle/payoff/PayoffVector.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::oracle {

struct PayoffMatrix {
    std::vector<std::string> asset_ids;
    std::vector<std::uint64_t> state_ids;
    std::vector<PayoffEntry> entries;

    std::uint32_t row_count = 0;
    std::uint32_t column_count = 0;
    std::uint64_t payoff_hash = 0;
};

}  // namespace trading_engine::oracle
