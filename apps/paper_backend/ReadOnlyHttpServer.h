#pragma once

#include "apps/paper_backend/DashboardApiRoutes.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace trading_engine::paper_backend {

enum class HttpMethod : std::uint8_t {
    Get,
    Head,
    Post,
    Put,
    Patch,
    Delete,
    Unknown
};

struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    std::string target = "/";
};

struct HttpResponse {
    std::uint16_t status = 500;
    std::string content_type = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

class ReadOnlyHttpServer {
public:
    ReadOnlyHttpServer();
    explicit ReadOnlyHttpServer(DashboardApiRoutes routes);

    [[nodiscard]] HttpResponse handle_request(const HttpRequest& request) const;

private:
    DashboardApiRoutes routes_;
};

[[nodiscard]] HttpMethod parse_http_method(std::string_view method) noexcept;
[[nodiscard]] bool method_allowed(HttpMethod method) noexcept;

}  // namespace trading_engine::paper_backend
