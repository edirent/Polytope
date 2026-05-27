#include "feed/raw_ingest/RawLogReader.h"
#include "feed/raw_ingest/RawLogWriter.h"
#include "feed/raw_ingest/RawPacket.h"
#include "feed/source_runtime/WebSocketClient.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using trading_engine::feed::Codec;
using trading_engine::feed::PacketFlags;
using trading_engine::feed::PacketHeartbeat;
using trading_engine::feed::PacketNone;
using trading_engine::feed::RawLogReader;
using trading_engine::feed::RawLogWriter;
using trading_engine::feed::SourceId;
using trading_engine::feed::WebSocketClient;
using trading_engine::feed::make_raw_packet;

constexpr std::uint64_t kNsPerMs = 1'000'000ULL;
constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;

struct Percentiles {
    std::uint64_t p50{0};
    std::uint64_t p95{0};
    std::uint64_t p99{0};
    std::uint64_t max{0};
};

struct CaptureLatencySamples {
    std::vector<std::uint64_t> make_raw_packet;
    std::vector<std::uint64_t> raw_write_packet;
    std::vector<std::uint64_t> total_on_message_capture;
};

struct Config {
    std::uint64_t seconds{300};
    std::string asset_id;
    std::string endpoint{
        "wss://ws-subscriptions-clob.polymarket.com/ws/market"
    };
    std::string raw_log_path{"logs/live_market.raw"};
    std::string jsonl_path{"logs/live_market.jsonl"};
    std::uint64_t ping_interval_ms{10'000};
    std::uint64_t stale_timeout_ms{30'000};
};

struct SmokeStats {
    std::atomic<std::uint64_t> messages_received{0};
    std::atomic<std::uint64_t> bytes_received{0};
    std::atomic<std::uint64_t> packets_written{0};
    std::atomic<std::uint64_t> ping_sent{0};
    std::atomic<std::uint64_t> pong_received{0};
    std::atomic<std::uint64_t> reconnect_count{0};
    std::atomic<std::uint64_t> raw_write_errors{0};
    std::atomic<std::uint64_t> transport_errors{0};
    std::atomic<std::uint64_t> last_message_received_ns{0};
};

std::uint64_t now_ns() {
    const auto now = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()
    );

    return static_cast<std::uint64_t>(ns.count());
}

std::uint64_t checked_sub(
    std::uint64_t end_ns,
    std::uint64_t start_ns
) noexcept {
    if (end_ns < start_ns) {
        return 0;
    }

    return end_ns - start_ns;
}

std::uint64_t percentile_value(
    std::vector<std::uint64_t> values,
    std::uint64_t numerator,
    std::uint64_t denominator
) {
    if (values.empty()) {
        return 0;
    }

    std::sort(values.begin(), values.end());

    const auto count = static_cast<std::uint64_t>(values.size());
    std::uint64_t index = (count * numerator + denominator - 1) / denominator;

    if (index == 0) {
        index = 1;
    }

    return values[static_cast<std::size_t>(index - 1)];
}

Percentiles summarize_latency(const std::vector<std::uint64_t>& values) {
    if (values.empty()) {
        return {};
    }

    return Percentiles{
        .p50 = percentile_value(values, 50, 100),
        .p95 = percentile_value(values, 95, 100),
        .p99 = percentile_value(values, 99, 100),
        .max = *std::max_element(values.begin(), values.end())
    };
}

bool is_pong_payload(const std::string& payload) {
    return payload == "PONG" ||
           payload == "pong" ||
           payload == "\"PONG\"" ||
           payload == "\"pong\"";
}

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());

    for (char c : value) {
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
                out.push_back(c);
                break;
        }
    }

    return out;
}

std::string subscription_message(const std::string& asset_id) {
    return
        std::string(R"({"assets_ids":[")") +
        json_escape(asset_id) +
        R"("],"type":"market","custom_feature_enabled":true})";
}

void prepare_output_file(const std::string& path) {
    const auto file_path = std::filesystem::path(path);
    const auto parent = file_path.parent_path();

    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::filesystem::remove(file_path);
}

bool validate_raw_log(
    const std::string& raw_log_path,
    std::uint64_t expected_packets
) {
    RawLogReader reader(raw_log_path);
    std::uint64_t packets_read = 0;

    while (true) {
        auto result = reader.next();
        if (result.eof()) {
            break;
        }

        if (!result.ok()) {
            return false;
        }

        ++packets_read;
    }

    return packets_read == expected_packets;
}

std::uint64_t last_message_age_ms(
    std::uint64_t now,
    std::uint64_t start,
    std::uint64_t last_message
) {
    if (last_message == 0) {
        return checked_sub(now, start) / kNsPerMs;
    }

    return checked_sub(now, last_message) / kNsPerMs;
}

void print_latency_block(
    const char* name,
    const std::vector<std::uint64_t>& samples
) {
    const auto summary = summarize_latency(samples);

    std::cout << "  " << name << ":\n";
    std::cout << "    p50: " << summary.p50 << '\n';
    std::cout << "    p95: " << summary.p95 << '\n';
    std::cout << "    p99: " << summary.p99 << '\n';
    std::cout << "    max: " << summary.max << '\n';
}

void print_report(
    const Config& config,
    const SmokeStats& stats,
    const CaptureLatencySamples& latency,
    std::uint64_t start_ns,
    std::uint64_t end_ns,
    bool validate_passed
) {
    const auto age_ms = last_message_age_ms(
        end_ns,
        start_ns,
        stats.last_message_received_ns.load()
    );

    std::cout << "live_feed_smoke:\n";
    std::cout << "  runtime_seconds: "
              << checked_sub(end_ns, start_ns) / kNsPerSecond << '\n';
    std::cout << "  messages_received: "
              << stats.messages_received.load() << '\n';
    std::cout << "  bytes_received: "
              << stats.bytes_received.load() << '\n';
    std::cout << "  packets_written: "
              << stats.packets_written.load() << '\n';
    std::cout << "  ping_sent: " << stats.ping_sent.load() << '\n';
    std::cout << "  pong_received: " << stats.pong_received.load() << '\n';
    std::cout << "  reconnect_count: " << stats.reconnect_count.load() << '\n';
    std::cout << "  raw_write_errors: "
              << stats.raw_write_errors.load() << '\n';
    std::cout << "  transport_errors: "
              << stats.transport_errors.load() << '\n';
    std::cout << "  last_message_age_ms: " << age_ms << '\n';
    std::cout << "  raw_log_path: " << config.raw_log_path << '\n';
    std::cout << "  jsonl_path: " << config.jsonl_path << '\n';
    std::cout << "  validate_raw_log_passed: "
              << (validate_passed ? "true" : "false") << '\n';

    std::cout << '\n';
    std::cout << "capture_latency_ns:\n";
    print_latency_block("make_raw_packet", latency.make_raw_packet);
    std::cout << '\n';
    print_latency_block("raw_write_packet", latency.raw_write_packet);
    std::cout << '\n';
    print_latency_block(
        "total_on_message_capture",
        latency.total_on_message_capture
    );
}

Config parse_args(int argc, char** argv) {
    Config config;

    if (const char* env_asset = std::getenv("POLYMARKET_ASSET_ID")) {
        config.asset_id = env_asset;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        const auto require_value = [&](const char* option) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(
                    std::string(option) + " requires a value"
                );
            }

            return argv[++i];
        };

        if (arg == "--seconds") {
            config.seconds = std::stoull(require_value("--seconds"));
            continue;
        }

        if (arg == "--asset-id") {
            config.asset_id = require_value("--asset-id");
            continue;
        }

        if (arg == "--endpoint") {
            config.endpoint = require_value("--endpoint");
            continue;
        }

        if (arg == "--raw-log-path") {
            config.raw_log_path = require_value("--raw-log-path");
            continue;
        }

        if (arg == "--jsonl-path") {
            config.jsonl_path = require_value("--jsonl-path");
            continue;
        }

        if (arg == "--help" || arg == "-h") {
            throw std::runtime_error(
                "usage: run_live_feed_smoke --seconds N --asset-id ASSET_ID "
                "[--raw-log-path PATH] [--jsonl-path PATH]"
            );
        }

        throw std::runtime_error("unknown argument: " + arg);
    }

    if (config.seconds == 0) {
        throw std::runtime_error("--seconds must be greater than zero");
    }

    if (config.asset_id.empty()) {
        throw std::runtime_error(
            "--asset-id is required unless POLYMARKET_ASSET_ID is set"
        );
    }

    return config;
}

int run(const Config& config) {
    prepare_output_file(config.raw_log_path);
    prepare_output_file(config.jsonl_path);

    RawLogWriter raw_writer(config.raw_log_path);
    std::ofstream jsonl_out(
        config.jsonl_path,
        std::ios::out | std::ios::trunc
    );

    if (!jsonl_out.is_open()) {
        throw std::runtime_error(
            "failed to open jsonl output: " + config.jsonl_path
        );
    }

    SmokeStats stats;
    CaptureLatencySamples latency;
    WebSocketClient client(config.endpoint);
    std::atomic<bool> fatal_error{false};
    std::atomic<bool> opened{false};
    std::mutex error_mutex;
    std::string last_error;
    std::atomic<std::uint64_t> next_packet_id{1};

    client.set_on_open([&]() {
        opened.store(true);
        client.send(subscription_message(config.asset_id));
    });

    client.set_on_message([&](const std::string& payload) {
        const auto t0 = now_ns();
        stats.last_message_received_ns.store(t0);
        stats.messages_received.fetch_add(1);
        stats.bytes_received.fetch_add(
            static_cast<std::uint64_t>(payload.size())
        );

        std::uint32_t flags = PacketNone;
        if (is_pong_payload(payload)) {
            flags |= static_cast<std::uint32_t>(PacketHeartbeat);
            stats.pong_received.fetch_add(1);
        }

        try {
            auto packet = make_raw_packet(
                SourceId::PolymarketMarket,
                1,
                next_packet_id.fetch_add(1),
                payload,
                Codec::None,
                flags
            );
            const auto t1 = now_ns();

            raw_writer.write_packet(packet);
            const auto t2 = now_ns();

            jsonl_out << packet.payload << '\n';
            if (!jsonl_out) {
                throw std::runtime_error("failed to write jsonl payload");
            }

            stats.packets_written.fetch_add(1);
            const auto t3 = now_ns();

            latency.make_raw_packet.push_back(checked_sub(t1, t0));
            latency.raw_write_packet.push_back(checked_sub(t2, t1));
            latency.total_on_message_capture.push_back(checked_sub(t3, t0));
        } catch (const std::exception& error) {
            stats.raw_write_errors.fetch_add(1);
            fatal_error.store(true);

            std::lock_guard<std::mutex> lock(error_mutex);
            last_error = error.what();
        }
    });

    client.set_on_error([&](const std::string& error) {
        stats.transport_errors.fetch_add(1);
        std::lock_guard<std::mutex> lock(error_mutex);
        last_error = error;
    });

    client.set_on_close([&]() {
        opened.store(false);
    });

    const auto start_ns = now_ns();

    client.connect();

    std::thread reader_thread([&]() {
        try {
            client.run();
        } catch (const std::exception& error) {
            stats.transport_errors.fetch_add(1);
            fatal_error.store(true);

            std::lock_guard<std::mutex> lock(error_mutex);
            last_error = error.what();
        }
    });

    std::uint64_t next_ping_ns = start_ns;
    const std::uint64_t ping_interval_ns =
        config.ping_interval_ms * kNsPerMs;
    const std::uint64_t deadline_ns =
        start_ns + config.seconds * kNsPerSecond;

    while (now_ns() < deadline_ns && !fatal_error.load()) {
        const auto now = now_ns();

        if (client.connected() && now >= next_ping_ns) {
            client.send("PING");
            stats.ping_sent.fetch_add(1);
            next_ping_ns = now + ping_interval_ns;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    client.disconnect();

    if (reader_thread.joinable()) {
        reader_thread.join();
    }

    raw_writer.flush();
    jsonl_out.flush();

    const auto end_ns = now_ns();
    const auto expected_packets = stats.packets_written.load();
    const bool validate_passed =
        validate_raw_log(config.raw_log_path, expected_packets);

    print_report(
        config,
        stats,
        latency,
        start_ns,
        end_ns,
        validate_passed
    );

    const auto age_ms = last_message_age_ms(
        end_ns,
        start_ns,
        stats.last_message_received_ns.load()
    );

    bool passed = true;
    passed &= opened.load() || stats.messages_received.load() > 0;
    passed &= stats.messages_received.load() > 0;
    passed &= stats.packets_written.load() == stats.messages_received.load();
    passed &= stats.raw_write_errors.load() == 0;
    passed &= validate_passed;
    passed &= age_ms <= config.stale_timeout_ms;
    passed &= !fatal_error.load();

    if (!passed) {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!last_error.empty()) {
            std::cerr << "run_live_feed_smoke last error: "
                      << last_error << '\n';
        }
    }

    return passed ? 0 : 1;
}

int fail(const std::string& message) {
    std::cerr << "run_live_feed_smoke failed: " << message << '\n';
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_args(argc, argv));
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}
