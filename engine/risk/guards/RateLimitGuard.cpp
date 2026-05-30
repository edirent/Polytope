#include "engine/risk/guards/RateLimitGuard.h"

namespace trading_engine::risk {

namespace {

constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;

}  // namespace

RateLimitGuard::RateLimitGuard(std::uint32_t max_per_second)
    : max_per_second_(max_per_second) {}

void RateLimitGuard::set_max_per_second(
    std::uint32_t max_per_second
) noexcept {
    max_per_second_ = max_per_second;
}

GuardResult RateLimitGuard::check(
    const signal::OpportunityIntent&,
    std::uint64_t now_ns
) {
    const std::uint64_t second = now_ns / kNsPerSecond;
    if (second != window_second_) {
        window_second_ = second;
        used_in_window_ = 0;
    }

    if (used_in_window_ < max_per_second_) {
        ++used_in_window_;
        return pass_guard();
    }

    GuardResult result;
    result.pass = false;
    result.rejection = RiskDecisionType::RejectRateLimited;
    result.reject_flag = kRiskRejectFlagRateLimited;
    result.reason = "risk approval rate limit exceeded";
    return result;
}

}  // namespace trading_engine::risk
