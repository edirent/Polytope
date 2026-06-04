#include "engine/execution/adapter/LiveOrderBridge.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
namespace execution = trading_engine::execution;

using Clock = std::chrono::steady_clock;

struct Config {
    std::string host{"clob.polymarket.com"};
    std::string port{"443"};
    std::string path{"/balance-allowance?asset_type=COLLATERAL"};
    std::string signature_path;
    std::uint64_t requests = 20;
    std::uint64_t interval_ms = 60;
    std::uint64_t timeout_seconds = 10;
    std::filesystem::path out_json;
    bool print_body = false;
    bool print_text_to_sign = false;
    std::size_t max_body_chars = 300;
    bool allow_any_get_path = false;
};

struct HttpSample {
    std::uint64_t total_us = 0;
    int status = 0;
    std::string body;
    std::string error;
};

struct LatencyStats {
    std::uint64_t count = 0;
    std::uint64_t min = 0;
    std::uint64_t p50 = 0;
    std::uint64_t p90 = 0;
    std::uint64_t p95 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t max = 0;
    double mean = 0.0;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::uint64_t elapsed_us(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start)
            .count()
    );
}

std::int64_t unix_time_seconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

std::optional<std::string> first_env(std::initializer_list<const char*> names) {
    for (const char* name : names) {
        const char* value = std::getenv(name);
        if (value != nullptr && value[0] != '\0') {
            return std::string{value};
        }
    }
    return std::nullopt;
}

execution::PolymarketL2Credentials load_credentials() {
    execution::PolymarketL2Credentials creds;
    creds.address = first_env({
        "POLYMARKET_L2_ADDRESS",
        "POLYMARKET_ADDRESS",
        "DEPOSIT_WALLET_ADDRESS",
        "FUNDER_ADDRESS"
    }).value_or("");
    creds.api_key = first_env({
        "POLYMARKET_L2_API_KEY",
        "CLOB_API_KEY",
        "API_KEY"
    }).value_or("");
    creds.secret = first_env({
        "POLYMARKET_L2_SECRET",
        "CLOB_SECRET",
        "SECRET"
    }).value_or("");
    creds.passphrase = first_env({
        "POLYMARKET_L2_PASSPHRASE",
        "CLOB_PASS_PHRASE",
        "CLOB_API_PASSPHRASE",
        "PASSPHRASE"
    }).value_or("");
    return creds;
}

std::string missing_credentials_message(
    const execution::PolymarketL2Credentials& creds
) {
    std::vector<std::string> missing;
    if (creds.address.empty()) {
        missing.emplace_back(
            "POLYMARKET_L2_ADDRESS "
            "(or POLYMARKET_ADDRESS/DEPOSIT_WALLET_ADDRESS/FUNDER_ADDRESS)"
        );
    }
    if (creds.api_key.empty()) {
        missing.emplace_back("POLYMARKET_L2_API_KEY (or CLOB_API_KEY/API_KEY)");
    }
    if (creds.secret.empty()) {
        missing.emplace_back("POLYMARKET_L2_SECRET (or CLOB_SECRET/SECRET)");
    }
    if (creds.passphrase.empty()) {
        missing.emplace_back(
            "POLYMARKET_L2_PASSPHRASE "
            "(or CLOB_PASS_PHRASE/CLOB_API_PASSPHRASE/PASSPHRASE)"
        );
    }

    std::string out{"missing L2 credentials:"};
    for (const auto& name : missing) {
        out += "\n  - ";
        out += name;
    }
    return out;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

bool is_safe_read_path(std::string_view path) {
    if (path.empty() || path.front() != '/') {
        return false;
    }
    if (starts_with(path, "/balance-allowance/update")) {
        return false;
    }
    return starts_with(path, "/balance-allowance") ||
           starts_with(path, "/data/orders") ||
           starts_with(path, "/data/trades") ||
           starts_with(path, "/data/balance") ||
           starts_with(path, "/orders") ||
           starts_with(path, "/trades") ||
           starts_with(path, "/notifications") ||
           starts_with(path, "/api-keys");
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += ' ';
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

std::string truncate_body(const std::string& body, std::size_t max_chars) {
    if (body.size() <= max_chars) {
        return body;
    }
    return body.substr(0, max_chars) + "...";
}

std::string default_signature_path(const std::string& request_path) {
    const auto query_pos = request_path.find('?');
    if (query_pos == std::string::npos) {
        return request_path;
    }
    return request_path.substr(0, query_pos);
}

Config parse_args(int argc, char** argv) {
    Config config;
    for (int index = 1; index < argc; ++index) {
        const std::string arg{argv[index]};
        auto value = [&](const char* option) -> std::string {
            if (index + 1 >= argc) {
                fail(std::string{"missing value for "} + option);
            }
            return argv[++index];
        };

        if (arg == "--host") {
            config.host = value("--host");
        } else if (arg == "--port") {
            config.port = value("--port");
        } else if (arg == "--path") {
            config.path = value("--path");
        } else if (arg == "--signature-path") {
            config.signature_path = value("--signature-path");
        } else if (arg == "--requests") {
            config.requests = std::stoull(value("--requests"));
        } else if (arg == "--interval-ms") {
            config.interval_ms = std::stoull(value("--interval-ms"));
        } else if (arg == "--timeout-seconds") {
            config.timeout_seconds = std::stoull(value("--timeout-seconds"));
        } else if (arg == "--out-json") {
            config.out_json = value("--out-json");
        } else if (arg == "--print-body") {
            config.print_body = true;
        } else if (arg == "--print-text-to-sign") {
            config.print_text_to_sign = true;
        } else if (arg == "--max-body-chars") {
            config.max_body_chars = std::stoull(value("--max-body-chars"));
        } else if (arg == "--allow-any-get-path") {
            config.allow_any_get_path = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "usage: probe_polymarket_auth_read "
                << "[--path /balance-allowance?asset_type=COLLATERAL] "
                << "[--signature-path /balance-allowance] "
                << "[--requests N] [--interval-ms MS] "
                << "[--out-json PATH] [--print-body] "
                << "[--print-text-to-sign]\n";
            std::exit(0);
        } else {
            fail("unknown argument: " + arg);
        }
    }

    if (config.requests == 0) {
        fail("--requests must be greater than zero");
    }
    if (config.timeout_seconds == 0) {
        fail("--timeout-seconds must be greater than zero");
    }
    if (!config.allow_any_get_path && !is_safe_read_path(config.path)) {
        fail(
            "refusing path outside the read-only allowlist: " + config.path +
            " (use --allow-any-get-path only after manual review)"
        );
    }
    if (!config.signature_path.empty() &&
        !config.allow_any_get_path &&
        !is_safe_read_path(config.signature_path)) {
        fail(
            "refusing signature path outside the read-only allowlist: " +
            config.signature_path +
            " (use --allow-any-get-path only after manual review)"
        );
    }
    return config;
}

HttpSample authenticated_get_once(
    const Config& config,
    const execution::PolymarketL2Authenticator& authenticator
) {
    HttpSample sample;
    const auto started = Clock::now();
    try {
        const auto timestamp = unix_time_seconds();
        const std::string path_to_sign = config.signature_path.empty()
            ? default_signature_path(config.path)
            : config.signature_path;
        const std::string text_to_sign =
            std::to_string(timestamp) + "GET" + path_to_sign;
        if (config.print_text_to_sign) {
            std::cout << "  text_to_sign: " << text_to_sign << '\n';
        }

        const auto headers = authenticator.build_headers(
            "GET",
            path_to_sign,
            "",
            timestamp
        );

        asio::io_context io;
        ssl::context tls(ssl::context::tls_client);
        tls.set_default_verify_paths();
        tls.set_verify_mode(ssl::verify_peer);

        tcp::resolver resolver(io);
        beast::ssl_stream<beast::tcp_stream> stream(io, tls);

        if (!SSL_set_tlsext_host_name(
                stream.native_handle(),
                config.host.c_str()
            )) {
            throw beast::system_error(
                beast::error_code(
                    static_cast<int>(::ERR_get_error()),
                    asio::error::get_ssl_category()
                )
            );
        }

        const auto results = resolver.resolve(config.host, config.port);
        beast::get_lowest_layer(stream).expires_after(
            std::chrono::seconds(config.timeout_seconds)
        );
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> request{
            http::verb::get,
            config.path,
            11
        };
        request.set(http::field::host, config.host);
        request.set(http::field::user_agent, "Polytope-auth-read-probe");
        request.set(http::field::accept, "application/json");
        request.set("POLY_ADDRESS", headers.address);
        request.set("POLY_API_KEY", headers.api_key);
        request.set("POLY_PASSPHRASE", headers.passphrase);
        request.set("POLY_TIMESTAMP", headers.timestamp);
        request.set("POLY_SIGNATURE", headers.signature);
        request.prepare_payload();

        http::write(stream, request);

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(stream, buffer, response);

        sample.status = response.result_int();
        sample.body = response.body();

        beast::error_code shutdown_error;
        stream.shutdown(shutdown_error);
        if (shutdown_error == asio::error::eof ||
            shutdown_error == ssl::error::stream_truncated) {
            shutdown_error = {};
        }
        if (shutdown_error) {
            throw beast::system_error(shutdown_error);
        }
    } catch (const std::exception& error) {
        sample.error = error.what();
    }
    sample.total_us = elapsed_us(started, Clock::now());
    return sample;
}

LatencyStats summarize(std::vector<std::uint64_t> values) {
    LatencyStats stats;
    if (values.empty()) {
        return stats;
    }
    std::sort(values.begin(), values.end());
    stats.count = values.size();
    stats.min = values.front();
    stats.max = values.back();
    auto percentile = [&](double p) -> std::uint64_t {
        const auto index = static_cast<std::size_t>(
            p * static_cast<double>(values.size() - 1) + 0.5
        );
        return values[std::min(index, values.size() - 1)];
    };
    stats.p50 = percentile(0.50);
    stats.p90 = percentile(0.90);
    stats.p95 = percentile(0.95);
    stats.p99 = percentile(0.99);
    long double total = 0.0;
    for (const auto value : values) {
        total += static_cast<long double>(value);
    }
    stats.mean = static_cast<double>(total / values.size());
    return stats;
}

void write_json_report(
    const Config& config,
    const std::vector<HttpSample>& samples,
    const LatencyStats& stats
) {
    if (config.out_json.empty()) {
        return;
    }
    if (!config.out_json.parent_path().empty()) {
        std::filesystem::create_directories(config.out_json.parent_path());
    }
    std::ofstream out(config.out_json);
    if (!out) {
        fail("failed to open --out-json: " + config.out_json.string());
    }

    std::uint64_t http_2xx = 0;
    std::uint64_t http_401 = 0;
    std::uint64_t http_non2xx = 0;
    std::uint64_t errors = 0;
    for (const auto& sample : samples) {
        if (!sample.error.empty()) {
            ++errors;
        } else if (sample.status >= 200 && sample.status < 300) {
            ++http_2xx;
        } else {
            ++http_non2xx;
            if (sample.status == 401) {
                ++http_401;
            }
        }
    }

    out << "{\n"
        << "  \"host\": \"" << json_escape(config.host) << "\",\n"
        << "  \"path\": \"" << json_escape(config.path) << "\",\n"
        << "  \"signature_path\": \""
        << json_escape(
               config.signature_path.empty()
                   ? default_signature_path(config.path)
                   : config.signature_path
           )
        << "\",\n"
        << "  \"connection_strategy\": \"new_tls_per_request\",\n"
        << "  \"requests\": " << samples.size() << ",\n"
        << "  \"http_2xx\": " << http_2xx << ",\n"
        << "  \"http_401\": " << http_401 << ",\n"
        << "  \"http_non2xx\": " << http_non2xx << ",\n"
        << "  \"transport_errors\": " << errors << ",\n"
        << "  \"latency_us\": {\n"
        << "    \"count\": " << stats.count << ",\n"
        << "    \"min\": " << stats.min << ",\n"
        << "    \"p50\": " << stats.p50 << ",\n"
        << "    \"p90\": " << stats.p90 << ",\n"
        << "    \"p95\": " << stats.p95 << ",\n"
        << "    \"p99\": " << stats.p99 << ",\n"
        << "    \"max\": " << stats.max << ",\n"
        << "    \"mean\": " << std::fixed << std::setprecision(2)
        << stats.mean << "\n"
        << "  },\n"
        << "  \"samples\": [\n";
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto& sample = samples[i];
        out << "    {\"index\": " << i
            << ", \"status\": " << sample.status
            << ", \"total_us\": " << sample.total_us
            << ", \"error\": \"" << json_escape(sample.error) << "\"";
        if (config.print_body) {
            out << ", \"body\": \""
                << json_escape(truncate_body(sample.body, config.max_body_chars))
                << "\"";
        }
        out << "}";
        if (i + 1 != samples.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n"
        << "}\n";
}

int run(int argc, char** argv) {
    const auto config = parse_args(argc, argv);
    const auto credentials = load_credentials();
    if (!credentials.complete()) {
        std::cerr << missing_credentials_message(credentials) << '\n';
        return 2;
    }

    execution::PolymarketL2Authenticator authenticator(credentials);
    std::vector<HttpSample> samples;
    samples.reserve(static_cast<std::size_t>(config.requests));

    for (std::uint64_t i = 0; i < config.requests; ++i) {
        samples.push_back(authenticated_get_once(config, authenticator));
        if (i + 1 != config.requests && config.interval_ms > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config.interval_ms)
            );
        }
    }

    std::vector<std::uint64_t> latency_values;
    latency_values.reserve(samples.size());
    std::uint64_t http_2xx = 0;
    std::uint64_t http_401 = 0;
    std::uint64_t http_non2xx = 0;
    std::uint64_t errors = 0;
    for (const auto& sample : samples) {
        if (!sample.error.empty()) {
            ++errors;
            continue;
        }
        latency_values.push_back(sample.total_us);
        if (sample.status >= 200 && sample.status < 300) {
            ++http_2xx;
        } else {
            ++http_non2xx;
            if (sample.status == 401) {
                ++http_401;
            }
        }
    }

    const auto stats = summarize(std::move(latency_values));
    write_json_report(config, samples, stats);

    std::cout << "polymarket_auth_read_probe:\n"
              << "  host: " << config.host << "\n"
              << "  path: " << config.path << "\n"
              << "  signature_path: "
              << (config.signature_path.empty()
                      ? default_signature_path(config.path)
                      : config.signature_path)
              << "\n"
              << "  connection_strategy: new_tls_per_request\n"
              << "  requests: " << samples.size() << "\n"
              << "  http_2xx: " << http_2xx << "\n"
              << "  http_401: " << http_401 << "\n"
              << "  http_non2xx: " << http_non2xx << "\n"
              << "  transport_errors: " << errors << "\n"
              << "  latency_us:\n"
              << "    count: " << stats.count << "\n"
              << "    min: " << stats.min << "\n"
              << "    p50: " << stats.p50 << "\n"
              << "    p90: " << stats.p90 << "\n"
              << "    p95: " << stats.p95 << "\n"
              << "    p99: " << stats.p99 << "\n"
              << "    max: " << stats.max << "\n"
              << "    mean: " << std::fixed << std::setprecision(2)
              << stats.mean << "\n";

    if (config.print_body) {
        for (std::size_t i = 0; i < samples.size(); ++i) {
            std::cout << "  sample_" << i << "_body: "
                      << truncate_body(samples[i].body, config.max_body_chars)
                      << "\n";
        }
    }

    if (!config.out_json.empty()) {
        std::cout << "  out_json: " << config.out_json << "\n";
    }

    if (http_401 > 0) {
        return 3;
    }
    if (http_2xx == 0) {
        return 4;
    }
    return errors == 0 ? 0 : 5;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "polymarket_auth_read_probe_error: " << error.what()
                  << '\n';
        return 1;
    }
}
