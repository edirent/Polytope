#pragma once

#include "state/chain/ConfirmedTradeState.h"
#include "state/quality/BookQualityState.h"
#include "state/StateTypes.h"

#include <array>
#include <cstdint>
#include <string>

namespace trading_engine::state {

struct MarketStateSnapshot {
    std::string entity_id;
    std::string market_id;
    std::uint64_t version{0};
    std::uint64_t last_book_update_ns{0};

    bool live{false};
    bool recovering{false};
    bool closed{false};
    bool resolved{false};
    bool crossed{false};

    bool has_bid{false};
    bool has_ask{false};

    std::int64_t best_bid_tick{0};
    std::int64_t best_ask_tick{0};

    std::uint32_t bid_count{0};
    std::uint32_t ask_count{0};

    std::array<PriceLevel, kMaxSnapshotDepth> bids{};
    std::array<PriceLevel, kMaxSnapshotDepth> asks{};

    std::string winning_asset_id;
    std::uint64_t state_hash{0};

    bool has_confirmed_trade{false};
    std::int64_t last_trade_price_tick{0};
    std::int64_t last_trade_size_lots{0};
    AggressorSide last_taker_side{AggressorSide::Unknown};

    std::int64_t confirmed_buy_lots_2s{0};
    std::int64_t confirmed_sell_lots_2s{0};
    std::int64_t confirmed_buy_lots_10s{0};
    std::int64_t confirmed_sell_lots_10s{0};

    BookQuality quality{BookQuality::Unknown};
    bool usable_for_depth{false};
    bool usable_for_signal{false};
};

}  // namespace trading_engine::state
