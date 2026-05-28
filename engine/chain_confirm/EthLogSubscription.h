#pragma once

#include <string>
#include <vector>

namespace trading_engine::chain_confirm {

struct EthLogSubscription {
    std::string contract_address;
    std::vector<std::string> topics;
    bool include_removed{true};
    bool narrow_filter{true};
};

}  // namespace trading_engine::chain_confirm
