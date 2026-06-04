#include "decode/core/DecodePipeline.h"
#include "decode/json/JsonDecodeResult.h"
#include "decode/public/NormalizedEventBatch.h"
#include "engine/risk/quote/QuoteRiskEvaluator.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/strategy/market_making/core/MarketMakingEngine.h"
#include "engine/strategy/market_making/public/CancelQuoteIntent.h"
#include "engine/strategy/market_making/public/MarketMakingConfig.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawPacket.h"
#include "feed/source_runtime/WebSocketClient.h"
#include "state/MarketStateView.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateStore.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
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
using tcp = asio::ip::tcp;

namespace decode = trading_engine::decode;
namespace feed = trading_engine::feed;
namespace mm = trading_engine::strategy::market_making;
namespace risk = trading_engine::risk;
namespace state = trading_engine::state;

using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;

struct Config {
    std::uint64_t seconds = 1'800;
    std::uint64_t ping_interval_ms = 10'000;
    std::string asset_id;
    std::string market_id{"world-cup"};
    std::string endpoint{
        "wss://ws-subscriptions-clob.polymarket.com/ws/market"
    };
    std::string mock_host{"127.0.0.1"};
    std::string mock_port{"8099"};
    std::string mock_path{"/orders"};
    std::filesystem::path out_json;
    bool require_real_signing = false;
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

struct Stats {
    std::atomic<std::uint64_t> ws_packets{0};
    std::atomic<std::uint64_t> normalized_events{0};
    std::atomic<std::uint64_t> filtered_events{0};
    std::atomic<std::uint64_t> book_snapshots{0};
    std::atomic<std::uint64_t> book_deltas{0};
    std::atomic<std::uint64_t> snapshots_published{0};
    std::atomic<std::uint64_t> depth_updates{0};
    std::atomic<std::uint64_t> mm_quote_intents{0};
    std::atomic<std::uint64_t> mm_cancel_intents{0};
    std::atomic<std::uint64_t> mm_rejected_no_quote{0};
    std::atomic<std::uint64_t> risk_evaluated{0};
    std::atomic<std::uint64_t> risk_approved{0};
    std::atomic<std::uint64_t> risk_rejected{0};
    std::atomic<std::uint64_t> unsigned_packets{0};
    std::atomic<std::uint64_t> loopback_posts{0};
    std::atomic<std::uint64_t> loopback_post_errors{0};
    std::atomic<std::uint64_t> decode_errors{0};
    std::atomic<std::uint64_t> state_errors{0};
    std::atomic<std::uint64_t> transport_errors{0};
    std::atomic<std::uint64_t> ping_sent{0};
    std::atomic<std::uint64_t> pong_received{0};
};

struct PostResult {
    bool ok = false;
    int status = 0;
    std::uint64_t latency_us = 0;
    std::string error;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::uint64_t now_ns() {
    const auto now = Clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
    );
}

std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count()
    );
}

std::uint64_t elapsed_us(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start)
            .count()
    );
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
                out.push_back(
                    static_cast<unsigned char>(c) < 0x20 ? ' ' : c
                );
                break;
        }
    }
    return out;
}

bool is_pong(const std::string& payload) {
    return payload == "PONG" || payload == "pong" ||
           payload == "\"PONG\"" || payload == "\"pong\"";
}

bool targets_other_asset(
    const trading_engine::decode::NormalizedEvent& event,
    const std::string& asset_id
) {
    if (event.event_type == decode::NormalizedEventType::Heartbeat) {
        return false;
    }
    const std::string& target = !event.asset_id.empty()
        ? event.asset_id
        : event.entity_id;
    return !target.empty() && target != asset_id;
}

std::string market_subscription(const std::string& asset_id) {
    return
        std::string{R"({"assets_ids":[")"} +
        json_escape(asset_id) +
        R"("],"type":"market","custom_feature_enabled":true})";
}

Config parse_args(int argc, char** argv) {
    Config config;
    if (const char* asset = std::getenv("POLYMARKET_ASSET_ID")) {
        config.asset_id = asset;
    }

    for (int index = 1; index < argc; ++index) {
        const std::string arg{argv[index]};
        auto value = [&](const char* option) -> std::string {
            if (index + 1 >= argc) {
                fail(std::string{"missing value for "} + option);
            }
            return argv[++index];
        };

        if (arg == "--seconds") {
            config.seconds = std::stoull(value("--seconds"));
        } else if (arg == "--asset-id") {
            config.asset_id = value("--asset-id");
        } else if (arg == "--market-id") {
            config.market_id = value("--market-id");
        } else if (arg == "--endpoint") {
            config.endpoint = value("--endpoint");
        } else if (arg == "--mock-host") {
            config.mock_host = value("--mock-host");
        } else if (arg == "--mock-port") {
            config.mock_port = value("--mock-port");
        } else if (arg == "--mock-path") {
            config.mock_path = value("--mock-path");
        } else if (arg == "--out-json") {
            config.out_json = value("--out-json");
        } else if (arg == "--require-real-signing") {
            config.require_real_signing = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "usage: run_market_making_loopback_replay "
                << "[--seconds 1800] [--asset-id ASSET] "
                << "[--mock-host 127.0.0.1] [--mock-port 8099] "
                << "[--out-json PATH]\n";
            std::exit(0);
        } else {
            fail("unknown argument: " + arg);
        }
    }

    if (config.seconds == 0) {
        fail("--seconds must be greater than zero");
    }
    if (config.asset_id.empty()) {
        fail("--asset-id is required unless POLYMARKET_ASSET_ID is set");
    }
    if (config.require_real_signing) {
        fail(
            "real signing is not wired into this replay target yet; "
            "run profile_live_order_signer first and remove this guard only "
            "after an EIP-712 OrderStructV2 signer is implemented"
        );
    }
    return config;
}

mm::MarketMakingConfig market_making_config() {
    mm::MarketMakingConfig config;
    config.strategy_id = 99;
    config.oracle_artifact_hash = 11;
    config.policy_hash = 22;
    config.min_half_spread_tick = mm::kDefaultDefensiveHalfSpreadTick;
    config.max_inventory_skew_tick = mm::kDefaultDefensiveInventorySkewTick;
    config.base_quote_size_lots = mm::kDefaultDefensiveQuoteSizeLots;
    config.max_inventory_lots = 100;
    config.quote_ttl_ns = 5'000'000'000ULL;
    config.requote_threshold_tick = 1'000;
    return config;
}

risk::QuoteRiskPolicy quote_risk_policy() {
    risk::QuoteRiskPolicy policy;
    policy.max_quote_qty_lots = 10;
    policy.max_quote_notional_tick = 10'000'000;
    policy.max_asset_inventory_lots = 100;
    policy.min_edge_to_fair_tick = -50'000;
    policy.max_book_age_ns = 1'000'000'000ULL;
    policy.min_replace_interval_ns = 0;
    policy.max_active_quotes_per_asset = 2;
    return policy;
}

std::string quote_leg_json(const mm::QuoteLeg& leg) {
    return std::string{"{\"market_id\":\""} + json_escape(leg.market_id) +
           "\",\"asset_id\":\"" + json_escape(leg.asset_id) +
           "\",\"side\":\"" +
           (leg.side == mm::QuoteSide::Bid ? "bid" : "ask") +
           "\",\"price_tick\":" + std::to_string(leg.price_tick) +
           ",\"quantity_lots\":" + std::to_string(leg.quantity_lots) +
           ",\"fair_value_tick\":" + std::to_string(leg.fair_value_tick) +
           ",\"edge_to_fair_tick\":" + std::to_string(leg.edge_to_fair_tick) +
           "}";
}

std::string approved_quote_packet(const risk::ApprovedQuote& quote) {
    std::string body =
        std::string{"{\"mode\":\"unsigned\",\"type\":\"approved_quote\""} +
        ",\"approved_quote_id\":" +
        std::to_string(quote.approved_quote_id) +
        ",\"quote_intent_id\":" +
        std::to_string(quote.quote_intent_id) +
        ",\"quote_group_id\":" +
        std::to_string(quote.quote_group_id) +
        ",\"has_bid\":" +
        (quote.has_bid ? "true" : "false") +
        ",\"has_ask\":" +
        (quote.has_ask ? "true" : "false");
    if (quote.has_bid) {
        body += ",\"bid\":" + quote_leg_json(quote.bid);
    }
    if (quote.has_ask) {
        body += ",\"ask\":" + quote_leg_json(quote.ask);
    }
    body += "}";
    return body;
}

std::string cancel_packet(const mm::CancelQuoteIntent& cancel) {
    return std::string{"{\"mode\":\"unsigned\",\"type\":\"cancel_quote\""} +
           ",\"cancel_intent_id\":" +
           std::to_string(cancel.cancel_intent_id) +
           ",\"quote_group_id\":" +
           std::to_string(cancel.quote_group_id) +
           ",\"active_quote_id\":" +
           std::to_string(cancel.active_quote_id) +
           ",\"asset_index\":" +
           std::to_string(cancel.asset_index) +
           "}";
}

PostResult post_local(
    const Config& config,
    const std::string& body
) {
    PostResult result;
    const auto started = Clock::now();
    try {
        asio::io_context io;
        tcp::resolver resolver(io);
        beast::tcp_stream stream(io);
        const auto resolved = resolver.resolve(config.mock_host, config.mock_port);
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(3));
        beast::get_lowest_layer(stream).connect(resolved);

        http::request<http::string_body> request{
            http::verb::post,
            config.mock_path,
            11
        };
        request.set(http::field::host, config.mock_host);
        request.set(http::field::user_agent, "Polytope-loopback-replay");
        request.set(http::field::content_type, "application/json");
        request.body() = body;
        request.prepare_payload();

        http::write(stream, request);
        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(stream, buffer, response);
        result.status = response.result_int();
        result.ok = result.status >= 200 && result.status < 300;
        beast::error_code ignored;
        stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    result.latency_us = elapsed_us(started, Clock::now());
    return result;
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

void write_latency_json(
    std::ostream& out,
    const char* name,
    const LatencyStats& stats,
    bool trailing_comma
) {
    out << "  \"" << name << "\": {\n"
        << "    \"count\": " << stats.count << ",\n"
        << "    \"min\": " << stats.min << ",\n"
        << "    \"p50\": " << stats.p50 << ",\n"
        << "    \"p90\": " << stats.p90 << ",\n"
        << "    \"p95\": " << stats.p95 << ",\n"
        << "    \"p99\": " << stats.p99 << ",\n"
        << "    \"max\": " << stats.max << ",\n"
        << "    \"mean\": " << std::fixed << std::setprecision(2)
        << stats.mean << "\n"
        << "  }" << (trailing_comma ? "," : "") << "\n";
}

void write_json_report(
    const Config& config,
    const Stats& stats,
    std::uint64_t runtime_seconds,
    const LatencyStats& pipeline_ns,
    const LatencyStats& post_us
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
    out << "{\n"
        << "  \"mode\": \"unsigned_loopback\",\n"
        << "  \"runtime_seconds\": " << runtime_seconds << ",\n"
        << "  \"asset_id\": \"" << json_escape(config.asset_id) << "\",\n"
        << "  \"ws_packets\": " << stats.ws_packets.load() << ",\n"
        << "  \"normalized_events\": " << stats.normalized_events.load()
        << ",\n"
        << "  \"filtered_events\": " << stats.filtered_events.load() << ",\n"
        << "  \"book_snapshots\": " << stats.book_snapshots.load() << ",\n"
        << "  \"book_deltas\": " << stats.book_deltas.load() << ",\n"
        << "  \"snapshots_published\": "
        << stats.snapshots_published.load() << ",\n"
        << "  \"depth_updates\": " << stats.depth_updates.load() << ",\n"
        << "  \"mm_quote_intents\": " << stats.mm_quote_intents.load()
        << ",\n"
        << "  \"mm_cancel_intents\": " << stats.mm_cancel_intents.load()
        << ",\n"
        << "  \"risk_approved\": " << stats.risk_approved.load() << ",\n"
        << "  \"risk_rejected\": " << stats.risk_rejected.load() << ",\n"
        << "  \"unsigned_packets\": " << stats.unsigned_packets.load()
        << ",\n"
        << "  \"loopback_posts\": " << stats.loopback_posts.load() << ",\n"
        << "  \"loopback_post_errors\": "
        << stats.loopback_post_errors.load() << ",\n"
        << "  \"decode_errors\": " << stats.decode_errors.load() << ",\n"
        << "  \"state_errors\": " << stats.state_errors.load() << ",\n"
        << "  \"transport_errors\": " << stats.transport_errors.load()
        << ",\n";
    write_latency_json(out, "pipeline_latency_ns", pipeline_ns, true);
    write_latency_json(out, "loopback_post_latency_us", post_us, false);
    out << "}\n";
}

void print_latency(const char* name, const LatencyStats& stats) {
    std::cout << "  " << name << ":\n"
              << "    count: " << stats.count << "\n"
              << "    min: " << stats.min << "\n"
              << "    p50: " << stats.p50 << "\n"
              << "    p90: " << stats.p90 << "\n"
              << "    p95: " << stats.p95 << "\n"
              << "    p99: " << stats.p99 << "\n"
              << "    max: " << stats.max << "\n"
              << "    mean: " << std::fixed << std::setprecision(2)
              << stats.mean << "\n";
}

void process_depth_update(
    const Config& config,
    const state::MarketDepthView& depth,
    mm::MarketMakingEngine* engine,
    risk::QuoteRiskEvaluator* risk_evaluator,
    const risk::QuoteRiskPolicy& policy,
    Stats* stats,
    std::vector<std::uint64_t>* post_latencies_us
) {
    const auto now = now_ns();
    const auto mm_result = engine->on_market_update(mm::MarketMakingInput{
        .market_id = config.market_id,
        .asset_id = config.asset_id,
        .market_index = 1,
        .asset_index = depth.asset_index,
        .depth = &depth,
        .current_position_lots = 0,
        .now_ns = now
    });

    stats->mm_quote_intents.fetch_add(mm_result.quote_count);
    stats->mm_cancel_intents.fetch_add(mm_result.cancel_count);
    stats->mm_rejected_no_quote.fetch_add(mm_result.rejected_no_quote);

    for (std::uint16_t i = 0; i < mm_result.quote_count; ++i) {
        const auto& quote = mm_result.quotes[i];
        stats->risk_evaluated.fetch_add(1);
        const auto risk_result = risk_evaluator->evaluate(risk::QuoteRiskInput{
            .quote = &quote,
            .depth = &depth,
            .policy = &policy,
            .current_position_lots = 0,
            .current_asset_exposure_tick = 0,
            .active_quotes_for_asset = 0,
            .last_replace_ts_ns = 0,
            .now_ns = now
        });
        if (!risk_result.approved_quote) {
            stats->risk_rejected.fetch_add(1);
            continue;
        }
        stats->risk_approved.fetch_add(1);
        const auto body = approved_quote_packet(*risk_result.approved_quote);
        stats->unsigned_packets.fetch_add(1);
        const auto posted = post_local(config, body);
        post_latencies_us->push_back(posted.latency_us);
        if (posted.ok) {
            stats->loopback_posts.fetch_add(1);
        } else {
            stats->loopback_post_errors.fetch_add(1);
        }
    }

    for (std::uint16_t i = 0; i < mm_result.cancel_count; ++i) {
        const auto body = cancel_packet(mm_result.cancels[i]);
        stats->unsigned_packets.fetch_add(1);
        const auto posted = post_local(config, body);
        post_latencies_us->push_back(posted.latency_us);
        if (posted.ok) {
            stats->loopback_posts.fetch_add(1);
        } else {
            stats->loopback_post_errors.fetch_add(1);
        }
    }
}

int run(const Config& config) {
    decode::DecodePipeline pipeline;
    state::MarketStateStore store;
    state::MarketStateView view(store);
    mm::MarketMakingEngine mm_engine(market_making_config());
    risk::QuoteRiskEvaluator risk_evaluator;
    const auto policy = quote_risk_policy();

    Stats stats;
    std::mutex state_mutex;
    std::atomic<bool> fatal{false};
    std::atomic<std::uint64_t> next_packet_id{1};
    std::vector<std::uint64_t> pipeline_latencies_ns;
    std::vector<std::uint64_t> post_latencies_us;

    feed::WebSocketClient market_ws(config.endpoint);

    market_ws.set_on_open([&]() {
        market_ws.send(market_subscription(config.asset_id));
    });

    market_ws.set_on_message([&](const std::string& payload) {
        const auto started = Clock::now();
        if (is_pong(payload)) {
            stats.pong_received.fetch_add(1);
        }

        const auto packet = feed::make_raw_packet(
            feed::SourceId::PolymarketMarket,
            1,
            next_packet_id.fetch_add(1),
            payload,
            feed::Codec::None,
            is_pong(payload)
                ? static_cast<std::uint32_t>(feed::PacketHeartbeat)
                : static_cast<std::uint32_t>(feed::PacketNone)
        );

        decode::NormalizedEventBatch batch;
        const auto decoded_result = pipeline.decode(
            feed::to_decode_input_view(packet),
            &batch
        );
        stats.ws_packets.fetch_add(1);
        stats.normalized_events.fetch_add(
            static_cast<std::uint64_t>(batch.size())
        );
        if (!decoded_result.ok() &&
            decoded_result.payload_kind != decode::JsonDecodeKind::NonJsonControl) {
            stats.decode_errors.fetch_add(1);
        }

        decode::NormalizedEventBatch asset_batch;
        for (const auto& event : batch.events) {
            if (targets_other_asset(event, config.asset_id)) {
                stats.filtered_events.fetch_add(1);
                continue;
            }
            if (event.event_type == decode::NormalizedEventType::Snapshot) {
                stats.book_snapshots.fetch_add(1);
            } else if (event.event_type == decode::NormalizedEventType::Delta) {
                stats.book_deltas.fetch_add(1);
            }
            static_cast<void>(asset_batch.push_back(event));
        }

        std::lock_guard<std::mutex> lock(state_mutex);
        bool published = false;
        for (const auto& event : state::from_normalized_batch(asset_batch)) {
            const auto result = store.apply(event);
            if (!result.ok()) {
                stats.state_errors.fetch_add(1);
            }
            if (result.snapshot_published) {
                stats.snapshots_published.fetch_add(1);
                published = true;
            }
        }

        if (published) {
            const std::array<std::string_view, 1> asset_ids{config.asset_id};
            const auto depth_batch = view.read_depth_batch_by_asset_id(asset_ids);
            if (depth_batch.ok && depth_batch.count > 0 &&
                depth_batch.views[0].usable_for_depth) {
                stats.depth_updates.fetch_add(1);
                process_depth_update(
                    config,
                    depth_batch.views[0],
                    &mm_engine,
                    &risk_evaluator,
                    policy,
                    &stats,
                    &post_latencies_us
                );
            }
        }

        pipeline_latencies_ns.push_back(elapsed_ns(started, Clock::now()));
    });

    market_ws.set_on_error([&](const std::string&) {
        stats.transport_errors.fetch_add(1);
    });

    const auto start_ns = now_ns();
    market_ws.connect();
    std::thread market_thread([&]() {
        try {
            market_ws.run();
        } catch (...) {
            stats.transport_errors.fetch_add(1);
            fatal.store(true);
        }
    });

    const auto deadline_ns = start_ns + config.seconds * kNsPerSecond;
    std::uint64_t next_ping_ns = start_ns;
    while (now_ns() < deadline_ns && !fatal.load()) {
        const auto now = now_ns();
        if (now >= next_ping_ns) {
            if (market_ws.connected()) {
                market_ws.send("PING");
                stats.ping_sent.fetch_add(1);
            }
            next_ping_ns = now + config.ping_interval_ms * 1'000'000ULL;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    market_ws.disconnect();
    if (market_thread.joinable()) {
        market_thread.join();
    }

    const auto runtime_seconds = (now_ns() - start_ns) / kNsPerSecond;
    const auto pipeline_stats = summarize(std::move(pipeline_latencies_ns));
    const auto post_stats = summarize(std::move(post_latencies_us));
    write_json_report(
        config,
        stats,
        runtime_seconds,
        pipeline_stats,
        post_stats
    );

    std::cout << "market_making_loopback_replay:\n"
              << "  mode: unsigned_loopback\n"
              << "  runtime_seconds: " << runtime_seconds << "\n"
              << "  asset_id: " << config.asset_id << "\n"
              << "  ws_packets: " << stats.ws_packets.load() << "\n"
              << "  normalized_events: " << stats.normalized_events.load()
              << "\n"
              << "  filtered_events: " << stats.filtered_events.load() << "\n"
              << "  book_snapshots: " << stats.book_snapshots.load() << "\n"
              << "  book_deltas: " << stats.book_deltas.load() << "\n"
              << "  snapshots_published: "
              << stats.snapshots_published.load() << "\n"
              << "  depth_updates: " << stats.depth_updates.load() << "\n"
              << "  mm_quote_intents: " << stats.mm_quote_intents.load()
              << "\n"
              << "  mm_cancel_intents: " << stats.mm_cancel_intents.load()
              << "\n"
              << "  mm_rejected_no_quote: "
              << stats.mm_rejected_no_quote.load() << "\n"
              << "  risk_evaluated: " << stats.risk_evaluated.load() << "\n"
              << "  risk_approved: " << stats.risk_approved.load() << "\n"
              << "  risk_rejected: " << stats.risk_rejected.load() << "\n"
              << "  unsigned_packets: " << stats.unsigned_packets.load()
              << "\n"
              << "  loopback_posts: " << stats.loopback_posts.load() << "\n"
              << "  loopback_post_errors: "
              << stats.loopback_post_errors.load() << "\n"
              << "  decode_errors: " << stats.decode_errors.load() << "\n"
              << "  state_errors: " << stats.state_errors.load() << "\n"
              << "  transport_errors: " << stats.transport_errors.load()
              << "\n"
              << "  ping_sent: " << stats.ping_sent.load() << "\n"
              << "  pong_received: " << stats.pong_received.load() << "\n";
    print_latency("pipeline_latency_ns", pipeline_stats);
    print_latency("loopback_post_latency_us", post_stats);
    if (!config.out_json.empty()) {
        std::cout << "  out_json: " << config.out_json << "\n";
    }
    return fatal.load() ? 2 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_args(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "market_making_loopback_replay_error: "
                  << error.what() << '\n';
        return 1;
    }
}
