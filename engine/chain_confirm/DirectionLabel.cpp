#include "chain_confirm/DirectionLabel.h"

namespace trading_engine::chain_confirm {

std::string to_string(HintDirection direction) {
    switch (direction) {
        case HintDirection::BuyAggressorHint:
            return "BuyAggressorHint";
        case HintDirection::SellAggressorHint:
            return "SellAggressorHint";
        case HintDirection::Unknown:
        default:
            return "Unknown";
    }
}

std::string to_string(HintSource source) {
    switch (source) {
        case HintSource::WsLastTradePrice:
            return "WsLastTradePrice";
        case HintSource::WsDepthDelta:
            return "WsDepthDelta";
        case HintSource::WsIntraPacketDepthMatch:
            return "WsIntraPacketDepthMatch";
        default:
            return "Unknown";
    }
}

std::string to_string(ConfirmedDirection direction) {
    switch (direction) {
        case ConfirmedDirection::BuyAggressor:
            return "BuyAggressor";
        case ConfirmedDirection::SellAggressor:
            return "SellAggressor";
        case ConfirmedDirection::Unknown:
        default:
            return "Unknown";
    }
}

}  // namespace trading_engine::chain_confirm
