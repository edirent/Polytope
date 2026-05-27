#pragma once

#include "feed/integrity/ConsistencyChecker.h"

namespace trading_engine::feed {

enum class RecoveryAction {
    None,
    Reconnect,
    Replay,
};

class RecoveryController {
public:
    [[nodiscard]] RecoveryAction decide(const ConsistencyReport& report, bool stale) const noexcept;
};

}  // namespace trading_engine::feed
