#include "state/chain/ConfirmedTradeState.h"

namespace trading_engine::state {

const char* to_string(AggressorSide side) noexcept {
    switch (side) {
        case AggressorSide::Buy:
            return "Buy";
        case AggressorSide::Sell:
            return "Sell";
        case AggressorSide::Unknown:
        default:
            return "Unknown";
    }
}

}  // namespace trading_engine::state
