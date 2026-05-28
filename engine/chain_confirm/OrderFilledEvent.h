#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::chain_confirm {

struct OrderFilledEvent {
    std::string order_hash;

    std::string maker;
    std::string taker;

    std::string maker_asset_id;
    std::string taker_asset_id;

    std::uint64_t maker_amount_filled{0};
    std::uint64_t taker_amount_filled{0};
    std::uint64_t fee{0};

    std::uint64_t block_number{0};
    std::string tx_hash;
    std::uint32_t log_index{0};

    bool removed{false};
};

}  // namespace trading_engine::chain_confirm
