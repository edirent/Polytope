#pragma once

#include "chain_confirm/DirectionLabel.h"

#include <cstdint>
#include <string>

namespace trading_engine::chain_confirm {

struct PendingTradeHint {
    std::uint64_t hint_id{0};

    std::string market_id;
    std::string asset_id;

    std::int64_t price_tick{0};
    std::int64_t size_lots{0};

    std::uint64_t ws_recv_monotonic_ns{0};
    std::uint64_t packet_id{0};
    std::uint64_t connection_id{0};

    HintDirection hint_direction{HintDirection::Unknown};
    HintSource source{HintSource::WsDepthDelta};

    std::uint8_t confidence_bps{0};

    bool finalized{false};
};

}  // namespace trading_engine::chain_confirm
