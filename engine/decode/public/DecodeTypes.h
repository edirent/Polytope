#pragma once

#include <cstdint>
#include <string_view>

namespace trading_engine::decode {

using PacketId = std::uint64_t;
using ConnectionId = std::uint64_t;

/**
 * @brief Identifies the logical data source that produced a packet/event.
 */
enum class SourceId : std::uint16_t {
    Unknown = 0,
    PolymarketMarket = 1,
    PolymarketUser = 2,
    PolymarketSports = 3,
    PolymarketRTDS = 4
};

struct DecodeInputView {
    PacketId packet_id{0};
    ConnectionId connection_id{0};
    std::uint64_t recv_wall_ns{0};
    std::uint64_t recv_monotonic_ns{0};
    SourceId source_id{SourceId::Unknown};
    std::uint16_t codec{0};
    std::uint32_t flags{0};
    std::string_view payload;
};

}  // namespace trading_engine::decode
