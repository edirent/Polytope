#include "state/quality/BookQualityState.h"

namespace trading_engine::state {

std::string to_string(BookQuality quality) {
    switch (quality) {
        case BookQuality::Good:
            return "Good";
        case BookQuality::Stale:
            return "Stale";
        case BookQuality::Recovering:
            return "Recovering";
        case BookQuality::Crossed:
            return "Crossed";
        case BookQuality::ChainMismatch:
            return "ChainMismatch";
        case BookQuality::ChainLagging:
            return "ChainLagging";
        case BookQuality::Closed:
            return "Closed";
        case BookQuality::Resolved:
            return "Resolved";
        case BookQuality::Unknown:
        default:
            return "Unknown";
    }
}

}  // namespace trading_engine::state
