#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::chain_confirm {

enum class HintDirection : std::uint8_t {
    Unknown = 0,
    BuyAggressorHint,
    SellAggressorHint
};

enum class HintSource : std::uint8_t {
    WsLastTradePrice = 0,
    WsDepthDelta,
    WsIntraPacketDepthMatch
};

enum class ConfirmedDirection : std::uint8_t {
    Unknown = 0,
    BuyAggressor,
    SellAggressor
};

[[nodiscard]] std::string to_string(HintDirection direction);
[[nodiscard]] std::string to_string(HintSource source);
[[nodiscard]] std::string to_string(ConfirmedDirection direction);

}  // namespace trading_engine::chain_confirm
