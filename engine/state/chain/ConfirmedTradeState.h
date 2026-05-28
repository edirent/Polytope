#pragma once

#include <cstdint>

namespace trading_engine::state {

enum class AggressorSide : std::uint8_t {
    Unknown = 0,
    Buy,
    Sell
};

struct ConfirmedTradeState {
    std::uint64_t version{0};

    std::uint64_t last_block_number{0};
    std::uint64_t last_chain_seen_ns{0};

    bool has_last_trade{false};
    std::int64_t last_trade_price_tick{0};
    std::int64_t last_trade_size_lots{0};
    AggressorSide last_taker_side{AggressorSide::Unknown};

    std::int64_t confirmed_buy_lots_2s{0};
    std::int64_t confirmed_sell_lots_2s{0};
    std::int64_t confirmed_buy_lots_10s{0};
    std::int64_t confirmed_sell_lots_10s{0};

    std::uint32_t unknown_fill_count_recent{0};
    std::uint32_t ambiguous_fill_count_recent{0};
    std::uint32_t removed_fill_count_recent{0};
};

[[nodiscard]] const char* to_string(AggressorSide side) noexcept;

}  // namespace trading_engine::state
