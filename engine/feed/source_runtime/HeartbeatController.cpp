#include "feed/source_runtime/HeartbeatController.h"

namespace trading_engine::feed {

void HeartbeatController::configure(std::chrono::milliseconds timeout) noexcept {
    timeout_ = timeout;
}

void HeartbeatController::mark_heartbeat(Clock::time_point now) noexcept {
    last_heartbeat_ = now;
}

bool HeartbeatController::expired(Clock::time_point now) const noexcept {
    return now - last_heartbeat_ > timeout_;
}

std::chrono::milliseconds HeartbeatController::timeout() const noexcept {
    return timeout_;
}

}  // namespace trading_engine::feed
