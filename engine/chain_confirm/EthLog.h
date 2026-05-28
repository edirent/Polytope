#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::chain_confirm {

struct EthLog {
    std::string address;
    std::vector<std::string> topics;
    std::string data;

    std::uint64_t block_number{0};
    std::string tx_hash;
    std::uint32_t log_index{0};

    bool removed{false};
};

}  // namespace trading_engine::chain_confirm
