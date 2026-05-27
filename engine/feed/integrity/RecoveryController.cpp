#include "feed/integrity/RecoveryController.h"

namespace trading_engine::feed {

RecoveryAction RecoveryController::decide(const ConsistencyReport& report, bool stale) const noexcept {
    if (!report.consistent) {
        return RecoveryAction::Replay;
    }

    if (stale) {
        return RecoveryAction::Reconnect;
    }

    return RecoveryAction::None;
}

}  // namespace trading_engine::feed
