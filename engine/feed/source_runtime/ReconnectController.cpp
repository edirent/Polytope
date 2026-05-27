#include "feed/source_runtime/ReconnectController.h"

#include <algorithm>

namespace trading_engine::feed {

void ReconnectController::record_failure() noexcept {
    ++attempts_;
}

void ReconnectController::reset() noexcept {
    attempts_ = 0;
}

std::chrono::milliseconds ReconnectController::next_delay() const noexcept {
    constexpr std::chrono::milliseconds base_delay{250};
    constexpr std::chrono::milliseconds max_delay{30000};
    const auto multiplier = 1U << std::min<std::uint32_t>(attempts_, 7);
    return std::min(base_delay * multiplier, max_delay);
}

std::uint32_t ReconnectController::attempts() const noexcept {
    return attempts_;
}

}  // namespace trading_engine::feed
