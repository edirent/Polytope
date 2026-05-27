#include "feed/output/HealthPublisher.h"

namespace trading_engine::feed {

void HealthPublisher::publish(HealthStatus status) noexcept {
    status_ = status;
}

HealthStatus HealthPublisher::status() const noexcept {
    return status_;
}

std::string_view HealthPublisher::status_text() const noexcept {
    switch (status_) {
        case HealthStatus::Starting:
            return "starting";
        case HealthStatus::Healthy:
            return "healthy";
        case HealthStatus::Degraded:
            return "degraded";
        case HealthStatus::Recovering:
            return "recovering";
        case HealthStatus::Stopped:
            return "stopped";
    }

    return "unknown";
}

}  // namespace trading_engine::feed
