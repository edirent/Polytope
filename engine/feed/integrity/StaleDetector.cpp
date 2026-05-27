#include "feed/integrity/StaleDetector.h"

namespace trading_engine::feed {

void StaleDetector::configure(std::chrono::milliseconds max_age) noexcept {
    max_age_ = max_age;
}

void StaleDetector::observe(Clock::time_point now) noexcept {
    last_observed_ = now;
}

bool StaleDetector::stale(Clock::time_point now) const noexcept {
    return now - last_observed_ > max_age_;
}

std::chrono::milliseconds StaleDetector::max_age() const noexcept {
    return max_age_;
}

}  // namespace trading_engine::feed
