#include "state/chain/SettlementState.h"

namespace trading_engine::state {

const char* to_string(SettlementStatus status) noexcept {
    switch (status) {
        case SettlementStatus::Open:
            return "Open";
        case SettlementStatus::Closed:
            return "Closed";
        case SettlementStatus::Resolved:
            return "Resolved";
        case SettlementStatus::Unknown:
        default:
            return "Unknown";
    }
}

}  // namespace trading_engine::state
