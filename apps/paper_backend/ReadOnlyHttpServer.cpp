#include "apps/paper_backend/ReadOnlyHttpServer.h"

#include <utility>

namespace trading_engine::paper_backend {
namespace {

[[nodiscard]] HttpResponse method_not_allowed() {
    HttpResponse response;
    response.status = 405;
    response.content_type = "application/json";
    response.body = R"({"error":"method_not_allowed"})";
    response.headers.push_back({"Allow", "GET, HEAD"});
    return response;
}

}  // namespace

ReadOnlyHttpServer::ReadOnlyHttpServer() = default;

ReadOnlyHttpServer::ReadOnlyHttpServer(DashboardApiRoutes routes)
    : routes_(std::move(routes)) {}

HttpResponse ReadOnlyHttpServer::handle_request(
    const HttpRequest& request
) const {
    if (!method_allowed(request.method)) {
        return method_not_allowed();
    }

    const auto route = routes_.handle_get(request.target);
    HttpResponse response;
    response.status = route.status;
    response.content_type = route.content_type;
    response.body = request.method == HttpMethod::Head ? "" : route.body;
    return response;
}

HttpMethod parse_http_method(std::string_view method) noexcept {
    if (method == "GET") {
        return HttpMethod::Get;
    }
    if (method == "HEAD") {
        return HttpMethod::Head;
    }
    if (method == "POST") {
        return HttpMethod::Post;
    }
    if (method == "PUT") {
        return HttpMethod::Put;
    }
    if (method == "PATCH") {
        return HttpMethod::Patch;
    }
    if (method == "DELETE") {
        return HttpMethod::Delete;
    }
    return HttpMethod::Unknown;
}

bool method_allowed(HttpMethod method) noexcept {
    return method == HttpMethod::Get || method == HttpMethod::Head;
}

}  // namespace trading_engine::paper_backend
