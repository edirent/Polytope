#pragma once

#include "engine/risk/guards/IRiskGuard.h"

namespace trading_engine::risk {

class KillSwitchGuard final : public IRiskGuard {
public:
    explicit KillSwitchGuard(bool enabled = false);

    void set_enabled(bool enabled) noexcept;

    GuardResult check(
        const signal::OpportunityIntent& intent,
        std::uint64_t now_ns
    ) override;

private:
    bool enabled_ = false;
};

}  // namespace trading_engine::risk
