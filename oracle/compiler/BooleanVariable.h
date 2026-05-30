#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::oracle {

struct BooleanVariable {
    std::uint32_t var_id = 0;

    std::string variable_key;
    std::string market_id;
    std::string outcome_id;
    std::string asset_id;
};

using BooleanVariableRegistry = std::vector<BooleanVariable>;

}  // namespace trading_engine::oracle
