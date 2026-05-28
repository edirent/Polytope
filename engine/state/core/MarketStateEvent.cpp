#include "state/core/MarketStateEvent.h"

namespace trading_engine::state {

std::string to_string(MarketStateEventType type) {
    switch (type) {
        case MarketStateEventType::WsBookSnapshot:
            return "WsBookSnapshot";
        case MarketStateEventType::WsBookDelta:
            return "WsBookDelta";
        case MarketStateEventType::WsHeartbeat:
            return "WsHeartbeat";
        case MarketStateEventType::WsLifecycle:
            return "WsLifecycle";
        case MarketStateEventType::ChainConfirmedFill:
            return "ChainConfirmedFill";
        case MarketStateEventType::ChainRemovedFill:
            return "ChainRemovedFill";
        case MarketStateEventType::ChainSettlement:
            return "ChainSettlement";
        case MarketStateEventType::DataQualityUpdate:
        default:
            return "DataQualityUpdate";
    }
}

}  // namespace trading_engine::state
