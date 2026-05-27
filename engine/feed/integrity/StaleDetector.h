#pragma once

#include <chrono>

namespace trading_engine::feed {

class StaleDetector {
public:
    using Clock = std::chrono::steady_clock;

    void configure(std::chrono::milliseconds max_age) noexcept;
    void observe(Clock::time_point now = Clock::now()) noexcept;

    [[nodiscard]] bool stale(Clock::time_point now = Clock::now()) const noexcept;
    [[nodiscard]] std::chrono::milliseconds max_age() const noexcept;

private:
    std::chrono::milliseconds max_age_{5000};
    Clock::time_point last_observed_{Clock::now()};
};

}  // namespace trading_engine::feed
