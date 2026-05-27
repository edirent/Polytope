#pragma once

#include <string_view>

namespace trading_engine::feed {

enum class HealthStatus {
    Starting,
    Healthy,
    Degraded,
    Recovering,
    Stopped,
};

class HealthPublisher {
public:
    void publish(HealthStatus status) noexcept;

    [[nodiscard]] HealthStatus status() const noexcept;
    [[nodiscard]] std::string_view status_text() const noexcept;

private:
    HealthStatus status_{HealthStatus::Starting};
};

}  // namespace trading_engine::feed
