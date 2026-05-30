#pragma once

#include "engine/risk/guards/IRiskGuard.h"

#include <cstdint>

namespace trading_engine::risk {

class RateLimitGuard final : public IRiskGuard {
public:
    explicit RateLimitGuard(std::uint32_t max_per_second);

    void set_max_per_second(std::uint32_t max_per_second) noexcept;

    GuardResult check(
        const signal::OpportunityIntent& intent,
        std::uint64_t now_ns
    ) override;

private:
    std::uint32_t max_per_second_ = 0;
    std::uint64_t window_second_ = 0;
    std::uint32_t used_in_window_ = 0;
};

}  // namespace trading_engine::risk
