#include "engine/risk/guards/KillSwitchGuard.h"

namespace trading_engine::risk {

KillSwitchGuard::KillSwitchGuard(bool enabled) : enabled_(enabled) {}

void KillSwitchGuard::set_enabled(bool enabled) noexcept {
    enabled_ = enabled;
}

GuardResult KillSwitchGuard::check(
    const signal::OpportunityIntent&,
    std::uint64_t
) {
    if (!enabled_) {
        return pass_guard();
    }

    GuardResult result;
    result.pass = false;
    result.rejection = RiskDecisionType::RejectKillSwitch;
    result.reject_flag = kRiskRejectFlagKillSwitch;
    result.reason = "kill switch enabled";
    return result;
}

}  // namespace trading_engine::risk
