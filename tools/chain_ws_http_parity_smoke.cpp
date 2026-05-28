#include "chain_confirm/EthLog.h"
#include "chain_confirm/OrderFilledDecoder.h"
#include "feed/source_runtime/WebSocketClient.h"

#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace json = boost::json;

using trading_engine::chain_confirm::EthLog;
using trading_engine::chain_confirm::OrderFilledDecoder;
using trading_engine::feed::WebSocketClient;

constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;

struct Config {
    std::uint64_t seconds{60};
    std::string polygon_ws_url;
    std::string polygon_http_url;
    std::string contract_address;
    std::uint64_t from_block{0};
    std::uint64_t to_block{0};
    std::uint64_t http_block_chunk{10};
    bool explicit_range{false};
};

struct Report {
    std::uint64_t start_block{0};
    std::uint64_t end_block{0};
    std::uint64_t ws_logs_seen{0};
    std::uint64_t http_logs_backfilled{0};
    std::uint64_t missing_from_ws{0};
    std::uint64_t extra_in_ws{0};
    std::uint64_t duplicates{0};
    std::uint64_t decode_errors{0};
    std::uint64_t removed_logs{0};
    bool subscription_opened{false};
    bool http_ok{false};
};

std::uint64_t now_ns() {
    const auto now = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count()
    );
}

std::string shell_quote(const std::string& value) {
    std::string out{"'"};
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::string curl_rpc(const std::string& url, const std::string& body) {
    const std::string command =
        "curl -sS --max-time 30 -H 'Content-Type: application/json' "
        "--data " + shell_quote(body) + " " + shell_quote(url);

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("failed to start curl");
    }

    std::string output;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }

    const int rc = pclose(pipe);
    if (rc != 0) {
        throw std::runtime_error("curl returned non-zero exit code");
    }
    return output;
}

std::string hex_quantity(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::uint64_t parse_hex_u64(const std::string& value) {
    std::size_t offset = 0;
    if (value.size() > 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        offset = 2;
    }
    std::uint64_t out = 0;
    for (; offset < value.size(); ++offset) {
        out <<= 4;
        const char c = value[offset];
        if (c >= '0' && c <= '9') {
            out += static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            out += static_cast<std::uint64_t>(10 + c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            out += static_cast<std::uint64_t>(10 + c - 'A');
        }
    }
    return out;
}

std::string escape_json(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

std::string log_filter_json(
    const std::string& contract_address,
    std::uint64_t from_block = 0,
    std::uint64_t to_block = 0,
    bool include_range = false
) {
    std::string filter{"{"};
    bool needs_comma = false;

    if (include_range) {
        filter += "\"fromBlock\":\"" + hex_quantity(from_block) + "\"";
        filter += ",\"toBlock\":\"" + hex_quantity(to_block) + "\"";
        needs_comma = true;
    }

    if (!contract_address.empty()) {
        if (needs_comma) {
            filter += ",";
        }
        filter += "\"address\":\"" + escape_json(contract_address) + "\"";
        needs_comma = true;
    }

    if (needs_comma) {
        filter += ",";
    }
    filter += "\"topics\":[\"";
    filter += OrderFilledDecoder::kOrderFilledTopic0;
    filter += "\"]}";
    return filter;
}

std::string subscription_message(const std::string& contract_address) {
    return
        std::string{
            R"({"jsonrpc":"2.0","id":1,"method":"eth_subscribe","params":["logs",)"
        } +
        log_filter_json(contract_address) + "]}";
}

std::string eth_get_logs_body(
    const std::string& contract_address,
    std::uint64_t from_block,
    std::uint64_t to_block
) {
    return
        std::string{R"({"jsonrpc":"2.0","id":1,"method":"eth_getLogs","params":[)"} +
        log_filter_json(contract_address, from_block, to_block, true) +
        R"(]})";
}

std::uint64_t eth_block_number(const std::string& http_url) {
    const std::string response = curl_rpc(
        http_url,
        R"({"jsonrpc":"2.0","id":1,"method":"eth_blockNumber","params":[]})"
    );
    boost::json::error_code error;
    const auto parsed = json::parse(response, error);
    if (error || !parsed.is_object()) {
        throw std::runtime_error("failed to parse eth_blockNumber response");
    }
    const auto& object = parsed.as_object();
    const auto it = object.find("result");
    if (it == object.end()) {
        throw std::runtime_error("eth_blockNumber missing result");
    }
    return parse_hex_u64(json::value_to<std::string>(it->value()));
}

std::string json_string_default(
    const json::object& object,
    const char* name
) {
    const auto it = object.find(name);
    if (it == object.end() || it->value().is_null()) {
        return {};
    }
    return json::value_to<std::string>(it->value());
}

bool json_bool_default(const json::object& object, const char* name) {
    const auto it = object.find(name);
    if (it == object.end() || it->value().is_null()) {
        return false;
    }
    return json::value_to<bool>(it->value());
}

EthLog eth_log_from_json(const json::object& object) {
    EthLog log;
    log.address = json_string_default(object, "address");
    log.data = json_string_default(object, "data");
    log.tx_hash = json_string_default(object, "transactionHash");
    log.block_number = parse_hex_u64(json_string_default(object, "blockNumber"));
    log.log_index = static_cast<std::uint32_t>(
        parse_hex_u64(json_string_default(object, "logIndex"))
    );
    log.removed = json_bool_default(object, "removed");

    const auto topics_it = object.find("topics");
    if (topics_it != object.end() && topics_it->value().is_array()) {
        for (const auto& topic : topics_it->value().as_array()) {
            log.topics.push_back(json::value_to<std::string>(topic));
        }
    }
    return log;
}

std::string log_key(const EthLog& log) {
    return log.tx_hash + ":" + std::to_string(log.log_index);
}

std::string json_rpc_error_message(const json::object& object) {
    const auto error_it = object.find("error");
    if (error_it == object.end() || !error_it->value().is_object()) {
        return {};
    }

    const auto& error = error_it->value().as_object();
    const auto message_it = error.find("message");
    if (message_it == error.end() || !message_it->value().is_string()) {
        return "JSON-RPC error";
    }

    return json::value_to<std::string>(message_it->value());
}

std::vector<EthLog> http_get_logs_range(
    const Config& config,
    std::uint64_t from_block,
    std::uint64_t to_block
) {
    const std::string response = curl_rpc(
        config.polygon_http_url,
        eth_get_logs_body(config.contract_address, from_block, to_block)
    );
    boost::json::error_code error;
    const auto parsed = json::parse(response, error);
    if (error || !parsed.is_object()) {
        throw std::runtime_error("failed to parse eth_getLogs response");
    }

    const auto& object = parsed.as_object();
    const auto it = object.find("result");
    if (it == object.end() || !it->value().is_array()) {
        const std::string rpc_error = json_rpc_error_message(object);
        if (!rpc_error.empty()) {
            throw std::runtime_error("eth_getLogs error: " + rpc_error);
        }
        throw std::runtime_error("eth_getLogs missing result array");
    }

    std::vector<EthLog> logs;
    for (const auto& item : it->value().as_array()) {
        if (item.is_object()) {
            logs.push_back(eth_log_from_json(item.as_object()));
        }
    }
    return logs;
}

std::vector<EthLog> http_get_logs(
    const Config& config,
    std::uint64_t from_block,
    std::uint64_t to_block
) {
    if (to_block < from_block) {
        return {};
    }

    const std::uint64_t chunk = std::max<std::uint64_t>(
        1,
        config.http_block_chunk
    );
    std::vector<EthLog> logs;

    for (std::uint64_t block = from_block; block <= to_block;) {
        const std::uint64_t end = std::min(
            to_block,
            block + chunk - 1
        );
        auto chunk_logs = http_get_logs_range(config, block, end);
        logs.insert(
            logs.end(),
            std::make_move_iterator(chunk_logs.begin()),
            std::make_move_iterator(chunk_logs.end())
        );

        if (end == std::numeric_limits<std::uint64_t>::max()) {
            break;
        }
        block = end + 1;
    }

    return logs;
}

Config parse_args(int argc, char** argv) {
    Config config;
    if (const char* ws = std::getenv("POLYGON_RPC_WS_URL")) {
        config.polygon_ws_url = ws;
    }
    if (const char* http = std::getenv("POLYGON_RPC_HTTP_URL")) {
        config.polygon_http_url = http;
    }
    if (const char* contract = std::getenv("POLYMARKET_CTF_EXCHANGE_ADDRESS")) {
        config.contract_address = contract;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](const char* option) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string(option) + " requires value");
            }
            return argv[++i];
        };
        if (arg == "--seconds") {
            config.seconds = std::stoull(value("--seconds"));
        } else if (arg == "--from-block") {
            config.from_block = std::stoull(value("--from-block"));
            config.explicit_range = true;
        } else if (arg == "--to-block") {
            config.to_block = std::stoull(value("--to-block"));
            config.explicit_range = true;
        } else if (arg == "--contract-address") {
            config.contract_address = value("--contract-address");
        } else if (arg == "--http-block-chunk") {
            config.http_block_chunk = std::stoull(value("--http-block-chunk"));
        } else if (arg == "--ws-url") {
            config.polygon_ws_url = value("--ws-url");
        } else if (arg == "--http-url") {
            config.polygon_http_url = value("--http-url");
        } else if (arg == "--help" || arg == "-h") {
            throw std::runtime_error(
                "usage: chain_ws_http_parity_smoke --seconds N "
                "[--contract-address ADDRESS] [--from-block N --to-block N]"
            );
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (config.polygon_ws_url.empty()) {
        throw std::runtime_error("POLYGON_RPC_WS_URL is required");
    }
    if (config.polygon_http_url.empty()) {
        throw std::runtime_error("POLYGON_RPC_HTTP_URL is required");
    }
    if (config.seconds == 0) {
        throw std::runtime_error("--seconds must be greater than zero");
    }
    if (config.http_block_chunk == 0) {
        throw std::runtime_error("--http-block-chunk must be greater than zero");
    }
    if (config.explicit_range && config.to_block < config.from_block) {
        throw std::runtime_error("--to-block must be >= --from-block");
    }
    return config;
}

void print_report(const Report& report) {
    std::cout << "chain_ws_http_parity_smoke:\n";
    std::cout << "  start_block: " << report.start_block << '\n';
    std::cout << "  end_block: " << report.end_block << '\n';
    std::cout << "  ws_logs_seen: " << report.ws_logs_seen << '\n';
    std::cout << "  http_logs_backfilled: "
              << report.http_logs_backfilled << '\n';
    std::cout << "  missing_from_ws: " << report.missing_from_ws << '\n';
    std::cout << "  extra_in_ws: " << report.extra_in_ws << '\n';
    std::cout << "  duplicates: " << report.duplicates << '\n';
    std::cout << "  decode_errors: " << report.decode_errors << '\n';
    std::cout << "  removed_logs: " << report.removed_logs << '\n';
    std::cout << "  subscription_opened: "
              << (report.subscription_opened ? "true" : "false") << '\n';
    std::cout << "  http_ok: " << (report.http_ok ? "true" : "false") << '\n';
}

int run(const Config& config) {
    Report report;
    std::set<std::string> ws_keys;
    std::set<std::string> duplicate_keys;
    std::atomic<bool> opened{false};
    std::atomic<std::uint64_t> decode_errors{0};
    std::atomic<std::uint64_t> removed_logs{0};
    std::mutex keys_mutex;

    const std::uint64_t start_block = config.explicit_range
        ? config.from_block
        : eth_block_number(config.polygon_http_url);

    WebSocketClient ws(config.polygon_ws_url);
    ws.set_on_open([&]() {
        opened.store(true);
        ws.send(subscription_message(config.contract_address));
    });
    ws.set_on_message([&](const std::string& payload) {
        boost::json::error_code error;
        const auto parsed = json::parse(payload, error);
        if (error || !parsed.is_object()) {
            decode_errors.fetch_add(1);
            return;
        }
        const auto& object = parsed.as_object();
        const auto params_it = object.find("params");
        if (params_it == object.end() || !params_it->value().is_object()) {
            return;
        }
        const auto& params = params_it->value().as_object();
        const auto result_it = params.find("result");
        if (result_it == params.end() || !result_it->value().is_object()) {
            return;
        }
        try {
            const EthLog log = eth_log_from_json(result_it->value().as_object());
            if (log.block_number < start_block) {
                return;
            }
            if (log.removed) {
                removed_logs.fetch_add(1);
            }
            std::lock_guard<std::mutex> lock(keys_mutex);
            const auto [_, inserted] = ws_keys.insert(log_key(log));
            if (!inserted) {
                duplicate_keys.insert(log_key(log));
            }
        } catch (...) {
            decode_errors.fetch_add(1);
        }
    });
    ws.set_on_error([&](const std::string&) {
        decode_errors.fetch_add(1);
    });

    ws.connect();
    std::thread ws_thread([&]() {
        try {
            ws.run();
        } catch (...) {
            decode_errors.fetch_add(1);
        }
    });

    const auto deadline = now_ns() + config.seconds * kNsPerSecond;
    while (now_ns() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ws.disconnect();
    if (ws_thread.joinable()) {
        ws_thread.join();
    }

    const std::uint64_t end_block = config.explicit_range
        ? config.to_block
        : eth_block_number(config.polygon_http_url);

    const auto http_logs = http_get_logs(config, start_block, end_block);
    std::set<std::string> http_keys;
    std::uint64_t http_removed = 0;
    for (const auto& log : http_logs) {
        http_keys.insert(log_key(log));
        if (log.removed) {
            ++http_removed;
        }
    }

    {
        std::lock_guard<std::mutex> lock(keys_mutex);
        report.ws_logs_seen = static_cast<std::uint64_t>(ws_keys.size());
        for (const auto& key : http_keys) {
            if (ws_keys.find(key) == ws_keys.end()) {
                ++report.missing_from_ws;
            }
        }
        for (const auto& key : ws_keys) {
            if (http_keys.find(key) == http_keys.end()) {
                ++report.extra_in_ws;
            }
        }
        report.duplicates = static_cast<std::uint64_t>(duplicate_keys.size());
    }

    report.start_block = start_block;
    report.end_block = end_block;
    report.http_logs_backfilled = static_cast<std::uint64_t>(http_logs.size());
    report.decode_errors = decode_errors.load();
    report.removed_logs = removed_logs.load() + http_removed;
    report.subscription_opened = opened.load();
    report.http_ok = true;
    print_report(report);

    const bool passed = report.subscription_opened &&
        report.http_ok &&
        report.decode_errors == 0;
    return passed ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_args(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "chain_ws_http_parity_smoke failed: "
                  << error.what() << '\n';
        return 1;
    }
}
