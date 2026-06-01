#pragma once

#include "engine/paper/read/DashboardReadStore.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace trading_engine::paper_backend {

struct ApiRouteResponse {
    std::uint16_t status = 404;
    std::string content_type = "application/json";
    std::string body = R"({"error":"not_found"})";
};

class DashboardApiRoutes {
public:
    DashboardApiRoutes() noexcept = default;

    explicit DashboardApiRoutes(
        const trading_engine::paper::DashboardReadStore* store
    ) noexcept;

    [[nodiscard]] ApiRouteResponse handle_get(
        std::string_view target
    ) const;

private:
    [[nodiscard]] ApiRouteResponse latest_snapshot() const;
    [[nodiscard]] ApiRouteResponse performance() const;
    [[nodiscard]] ApiRouteResponse regime() const;
    [[nodiscard]] ApiRouteResponse latency() const;

    const trading_engine::paper::DashboardReadStore* store_ = nullptr;
};

[[nodiscard]] std::string dashboard_snapshot_json(
    const trading_engine::paper::DashboardSnapshot& snapshot
);

}  // namespace trading_engine::paper_backend
