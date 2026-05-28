#pragma once

#include "chain_confirm/ClassifiedFillRecord.h"
#include "decode/public/NormalizedEvent.h"

#include <cstdint>
#include <string>

namespace trading_engine::state {

enum class MarketStateEventType : std::uint8_t {
    WsBookSnapshot = 0,
    WsBookDelta,
    WsHeartbeat,
    WsLifecycle,

    ChainConfirmedFill,
    ChainRemovedFill,
    ChainSettlement,

    DataQualityUpdate
};

struct MarketStateEvent {
    MarketStateEventType type{MarketStateEventType::DataQualityUpdate};

    std::string market_id;
    std::string asset_id;

    std::uint64_t recv_monotonic_ns{0};
    std::uint64_t source_sequence{0};

    decode::NormalizedEvent ws_event;
    chain_confirm::ClassifiedFillRecord chain_fill;
};

[[nodiscard]] std::string to_string(MarketStateEventType type);

}  // namespace trading_engine::state
