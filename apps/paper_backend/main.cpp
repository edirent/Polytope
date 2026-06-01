#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
using tcp = asio::ip::tcp;

struct Config {
    std::string host{"127.0.0.1"};
    std::uint16_t port = 8080;
    std::filesystem::path dashboard_file;
    std::filesystem::path pid_file;
};

struct PaperProcessStatus {
    bool running = false;
    long pid = 0;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

[[nodiscard]] std::string read_file_if_exists(
    const std::filesystem::path& path
) {
    if (path.empty()) {
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
}

[[nodiscard]] std::string path_only(std::string_view target) {
    const auto question = target.find('?');
    return std::string{target.substr(0, question)};
}

[[nodiscard]] bool is_write_method(http::verb method) noexcept {
    return method == http::verb::post ||
           method == http::verb::put ||
           method == http::verb::patch ||
           method == http::verb::delete_;
}

[[nodiscard]] bool method_allowed(http::verb method) noexcept {
    return method == http::verb::get || method == http::verb::head;
}

[[nodiscard]] bool is_stream_target(std::string_view target) {
    return path_only(target) == "/stream/v1/dashboard";
}

[[nodiscard]] PaperProcessStatus paper_process_status(
    const std::filesystem::path& pid_file
) {
    PaperProcessStatus status;
    const auto text = read_file_if_exists(pid_file);
    if (text.empty()) {
        return status;
    }

    try {
        status.pid = std::stol(text);
    } catch (...) {
        return {};
    }

    if (status.pid <= 0) {
        return status;
    }

    errno = 0;
    status.running = (::kill(static_cast<pid_t>(status.pid), 0) == 0) ||
                     errno == EPERM;
    return status;
}

[[nodiscard]] std::optional<json::object> dashboard_object(
    const std::filesystem::path& dashboard_file
) {
    const auto text = read_file_if_exists(dashboard_file);
    if (text.empty()) {
        return std::nullopt;
    }

    boost::json::error_code error;
    auto parsed = json::parse(text, error);
    if (error || !parsed.is_object()) {
        return std::nullopt;
    }
    return parsed.as_object();
}

[[nodiscard]] const json::object* nested_object(
    const json::object& object,
    const char* key
) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || !value->is_object()) {
        return nullptr;
    }
    return &value->as_object();
}

[[nodiscard]] std::int64_t get_i64(
    const json::object& object,
    const char* key
) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || value->is_null()) {
        return 0;
    }
    if (value->is_int64()) {
        return value->as_int64();
    }
    if (value->is_uint64()) {
        return static_cast<std::int64_t>(value->as_uint64());
    }
    return 0;
}

[[nodiscard]] std::string json_object_or_default(
    const std::optional<json::object>& dashboard,
    const char* key,
    std::string fallback
) {
    if (!dashboard) {
        return fallback;
    }
    const auto* object = nested_object(*dashboard, key);
    if (object == nullptr) {
        return fallback;
    }
    return json::serialize(*object);
}

[[nodiscard]] std::string json_array_or_default(
    const std::optional<json::object>& dashboard,
    const char* key,
    std::string fallback
) {
    if (!dashboard) {
        return fallback;
    }
    const auto* value = dashboard->if_contains(key);
    if (value == nullptr || !value->is_array()) {
        return fallback;
    }
    return json::serialize(value->as_array());
}

[[nodiscard]] std::string health_json(const Config& config) {
    const auto process = paper_process_status(config.pid_file);
    std::ostringstream out;
    out << "{\"ok\":true,\"mode\":\"readonly\""
        << ",\"paper_trading_running\":"
        << (process.running ? "true" : "false")
        << ",\"paper_trading_pid\":" << process.pid
        << ",\"dashboard_file\":\"" << config.dashboard_file.string()
        << "\"}";
    return out.str();
}

[[nodiscard]] std::string equity_json(
    const std::optional<json::object>& dashboard
) {
    if (!dashboard) {
        return R"({"equity_mid":0,"equity_liquidation":0})";
    }
    const auto* account = nested_object(*dashboard, "account");
    if (account == nullptr) {
        return R"({"equity_mid":0,"equity_liquidation":0})";
    }
    const auto equity = get_i64(*account, "starting_cash_tick") +
                        get_i64(*account, "realized_pnl_tick") +
                        get_i64(*account, "unrealized_pnl_tick");
    std::ostringstream out;
    out << "{\"equity_mid\":" << equity
        << ",\"equity_liquidation\":" << equity << "}";
    return out.str();
}

[[nodiscard]] std::string route_body(
    const Config& config,
    std::string_view target,
    std::string* content_type
) {
    const auto path = path_only(target);
    const auto dashboard = dashboard_object(config.dashboard_file);
    *content_type = "application/json";

    if (path == "/api/v1/health") {
        return health_json(config);
    }
    if (path == "/api/v1/snapshot/latest") {
        if (!dashboard) {
            return R"({"snapshot":null})";
        }
        return json::serialize(*dashboard);
    }
    if (path == "/api/v1/markets") {
        return R"({"markets":[]})";
    }
    if (path == "/api/v1/intents") {
        return R"({"intents":[]})";
    }
    if (path == "/api/v1/risk-decisions") {
        return R"({"risk_decisions":[]})";
    }
    if (path == "/api/v1/execution-reports") {
        return std::string{"{\"execution_reports\":"} +
               json_array_or_default(dashboard, "filled_orders", "[]") + "}";
    }
    if (path == "/api/v1/pnl/equity") {
        return equity_json(dashboard);
    }
    if (path == "/api/v1/performance") {
        return json_object_or_default(
            dashboard,
            "performance",
            R"({"intents_observed":0,"approvals_observed":0,"plans_observed":0,"execution_reports_observed":0,"filled_plans":0,"failed_plans":0,"gross_pnl_tick":0,"net_pnl_tick":0,"terminal_payout_tick":0,"terminal_cost_tick":0,"terminal_pnl_tick":0,"terminal_complete_plans":0,"max_drawdown_tick":0,"version":0,"updated_ts_ns":0})"
        );
    }
    if (path == "/api/v1/regime") {
        return json_object_or_default(
            dashboard,
            "regime",
            R"({"data":"Unknown","liquidity":"Unknown","chain":"Unknown","signal":"Unknown","risk":"Unknown","execution":"Unknown","version":0,"ts_ns":0})"
        );
    }
    if (path == "/api/v1/latency") {
        return json_object_or_default(
            dashboard,
            "latency",
            R"({"feed_to_state_ns":0,"state_to_signal_ns":0,"signal_to_risk_ns":0,"risk_to_execution_ns":0,"end_to_end_ns":0})"
        );
    }
    if (path == "/stream/v1/dashboard") {
        *content_type = "text/event-stream";
        if (!dashboard) {
            return ": ready\n\n";
        }
        return "event: dashboard\ndata: " + json::serialize(*dashboard) + "\n\n";
    }

    return R"({"error":"not_found"})";
}

[[nodiscard]] http::response<http::string_body> handle_request(
    const Config& config,
    const http::request<http::string_body>& request
) {
    http::response<http::string_body> response;
    response.version(request.version());
    response.keep_alive(false);
    response.set(http::field::access_control_allow_origin, "*");
    response.set(http::field::cache_control, "no-store");

    if (is_write_method(request.method()) || !method_allowed(request.method())) {
        response.result(http::status::method_not_allowed);
        response.set(http::field::allow, "GET, HEAD");
        response.set(http::field::content_type, "application/json");
        response.body() = R"({"error":"method_not_allowed"})";
        response.prepare_payload();
        return response;
    }

    std::string content_type;
    const auto body = route_body(config, request.target(), &content_type);
    response.result(
        body == R"({"error":"not_found"})"
            ? http::status::not_found
            : http::status::ok
    );
    response.set(http::field::content_type, content_type);
    if (content_type == "text/event-stream") {
        response.set(http::field::cache_control, "no-cache");
    }
    if (request.method() != http::verb::head) {
        response.body() = body;
    }
    response.prepare_payload();
    return response;
}

void serve_stream(
    tcp::socket socket,
    const Config& config,
    unsigned version
) {
    boost::system::error_code error;

    const auto status_line = version == 10 ? "HTTP/1.0 200 OK\r\n"
                                           : "HTTP/1.1 200 OK\r\n";
    const std::string header =
        std::string{status_line} +
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: keep-alive\r\n\r\n";
    asio::write(socket, asio::buffer(header), error);
    if (error) {
        return;
    }

    std::string last_payload;
    for (;;) {
        const auto dashboard = read_file_if_exists(config.dashboard_file);
        auto payload = dashboard.empty()
            ? std::string{": keepalive\n\n"}
            : std::string{"event: dashboard\ndata: "} + dashboard + "\n\n";

        if (payload != last_payload) {
            asio::write(socket, asio::buffer(payload), error);
            if (error) {
                return;
            }
            last_payload = std::move(payload);
        } else {
            const std::string keepalive{": keepalive\n\n"};
            asio::write(socket, asio::buffer(keepalive), error);
            if (error) {
                return;
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void serve_session(tcp::socket socket, Config config) {
    beast::flat_buffer buffer;
    boost::system::error_code error;
    http::request<http::string_body> request;
    http::read(socket, buffer, request, error);
    if (!error) {
        if (request.method() == http::verb::get &&
            is_stream_target(request.target())) {
            serve_stream(std::move(socket), config, request.version());
            return;
        }
        auto response = handle_request(config, request);
        http::write(socket, response, error);
    }
    socket.shutdown(tcp::socket::shutdown_send, error);
}

[[nodiscard]] Config parse_args(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto require_value = [&](const char* option) -> std::string {
            if (i + 1 >= argc) {
                fail(std::string(option) + " requires value");
            }
            return argv[++i];
        };

        if (arg == "--host") {
            config.host = require_value("--host");
        } else if (arg == "--port") {
            config.port = static_cast<std::uint16_t>(
                std::stoul(require_value("--port"))
            );
        } else if (arg == "--dashboard-file") {
            config.dashboard_file = require_value("--dashboard-file");
        } else if (arg == "--pid-file") {
            config.pid_file = require_value("--pid-file");
        } else if (arg == "--help" || arg == "-h") {
            fail(
                "usage: paper_backend --host 127.0.0.1 --port 8080 "
                "--dashboard-file PATH --pid-file PATH"
            );
        } else {
            fail("unknown argument: " + arg);
        }
    }
    return config;
}

int run(const Config& config) {
    asio::io_context io;
    tcp::endpoint endpoint{asio::ip::make_address(config.host), config.port};
    tcp::acceptor acceptor{io};
    acceptor.open(endpoint.protocol());
    acceptor.set_option(asio::socket_base::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen(asio::socket_base::max_listen_connections);

    std::cout << "paper_backend listening on http://"
              << config.host << ':' << config.port << '\n';

    for (;;) {
        tcp::socket socket{io};
        acceptor.accept(socket);
        std::thread{serve_session, std::move(socket), config}.detach();
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_args(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "paper_backend failed: " << error.what() << '\n';
        return 1;
    }
}
