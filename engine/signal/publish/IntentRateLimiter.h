#pragma once

#include <cstdint>

namespace trading_engine::signal {

class IntentRateLimiter {
public:
    explicit IntentRateLimiter(std::int32_t max_intents_per_second);

    [[nodiscard]] bool allow(std::uint64_t now_ns);

private:
    std::int32_t max_intents_per_second_ = 0;
    std::uint64_t current_second_ = 0;
    std::int32_t emitted_this_second_ = 0;
    bool has_window_ = false;
};

}  // namespace trading_engine::signal
