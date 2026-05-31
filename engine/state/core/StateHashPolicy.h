#pragma once

#include <cstdint>

namespace trading_engine::state {

enum class StateHashMode : std::uint8_t {
    HotPathLight,
    ReplayFull,
    DebugVerify
};

struct StateRuntimeConfig {
    StateHashMode hash_mode = StateHashMode::HotPathLight;

    bool publish_on_noop = false;
    bool publish_on_heartbeat = false;
    bool compute_full_hash_on_publish = false;
};

}  // namespace trading_engine::state
