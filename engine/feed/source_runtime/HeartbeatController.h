#pragma once

#include <chrono>

namespace trading_engine::feed {

class HeartbeatController {
public:
    using Clock = std::chrono::steady_clock;

    void configure(std::chrono::milliseconds timeout) noexcept;
    void mark_heartbeat(Clock::time_point now = Clock::now()) noexcept;

    [[nodiscard]] bool expired(Clock::time_point now = Clock::now()) const noexcept;
    [[nodiscard]] std::chrono::milliseconds timeout() const noexcept;

private:
    std::chrono::milliseconds timeout_{30000};
    Clock::time_point last_heartbeat_{Clock::now()};
};

}  // namespace trading_engine::feed
