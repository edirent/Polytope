#include "engine/signal/publish/IntentRateLimiter.h"

namespace trading_engine::signal {

namespace {

constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;

}  // namespace

IntentRateLimiter::IntentRateLimiter(
    std::int32_t max_intents_per_second
) : max_intents_per_second_(max_intents_per_second) {}

bool IntentRateLimiter::allow(std::uint64_t now_ns) {
    const auto second = now_ns / kNsPerSecond;
    if (!has_window_ || second != current_second_) {
        current_second_ = second;
        emitted_this_second_ = 0;
        has_window_ = true;
    }

    if (max_intents_per_second_ <= 0) {
        return false;
    }
    if (emitted_this_second_ >= max_intents_per_second_) {
        return false;
    }

    ++emitted_this_second_;
    return true;
}

}  // namespace trading_engine::signal
