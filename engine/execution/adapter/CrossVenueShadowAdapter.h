#pragma once
#include "engine/state/cross_venue/Types.h"

namespace trading_engine::cross_venue {
// Deliberately has no credential fields, live transport, or live-enable method.
struct CrossVenueShadowAdapter {
    static constexpr bool live_enabled = false;
    static void submit_order() { throw std::logic_error("cross-venue V0 is read-only; order transport unavailable"); }
};
}  // namespace trading_engine::cross_venue
