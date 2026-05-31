#pragma once

#include <cstdint>

namespace trading_engine::state {

enum class StateMutationKind : std::uint8_t {
    None,
    BookSnapshot,
    BookDelta,
    ChainFill,
    ChainRemovedFill,
    Lifecycle,
    Quality,
    Heartbeat
};

struct StateMutationFlags {
    bool state_changed = false;
    bool book_changed = false;
    bool chain_changed = false;
    bool quality_changed = false;
    bool lifecycle_changed = false;
    bool heartbeat_seen = false;

    bool publish_required = false;

    StateMutationKind kind = StateMutationKind::None;
};

}  // namespace trading_engine::state
