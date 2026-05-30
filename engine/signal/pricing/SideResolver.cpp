#include "engine/signal/pricing/SideResolver.h"

namespace trading_engine::signal {

ExecutableBookSide SideResolver::resolve(
    trading_engine::oracle::Side side
) const noexcept {
    switch (side) {
        case trading_engine::oracle::Side::Buy:
            return ExecutableBookSide::Asks;
        case trading_engine::oracle::Side::Sell:
            return ExecutableBookSide::Unsupported;
    }
    return ExecutableBookSide::Unsupported;
}

}  // namespace trading_engine::signal
