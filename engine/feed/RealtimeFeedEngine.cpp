#include "feed/RealtimeFeedEngine.h"

namespace trading_engine::feed {

RealtimeFeedEngine::RealtimeFeedEngine() = default;

void RealtimeFeedEngine::start() {
    running_ = true;
    heartbeat_controller_.mark_heartbeat();
    stale_detector_.observe();
    health_publisher_.publish(HealthStatus::Healthy);
}

void RealtimeFeedEngine::stop() {
    websocket_client_.disconnect();
    running_ = false;
    health_publisher_.publish(HealthStatus::Stopped);
}

bool RealtimeFeedEngine::running() const noexcept {
    return running_;
}

}  // namespace trading_engine::feed
