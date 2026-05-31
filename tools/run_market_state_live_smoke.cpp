#include "chain_confirm/EthLog.h"
#include "chain_confirm/OrderFilledDecoder.h"
#include "decode/core/DecodePipeline.h"
#include "decode/json/JsonDecodeResult.h"
#include "decode/public/NormalizedEventBatch.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawPacket.h"
#include "feed/source_runtime/WebSocketClient.h"
#include "state/MarketStateView.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateStore.h"

#include <boost/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace json = boost::json;

using trading_engine::chain_confirm::ConfirmedDirection;
using trading_engine::chain_confirm::EthLog;
using trading_engine::chain_confirm::OrderFilledDecoder;
using trading_engine::decode::DecodePipeline;
using trading_engine::decode::JsonDecodeKind;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;
using trading_engine::feed::Codec;
using trading_engine::feed::PacketHeartbeat;
using trading_engine::feed::PacketNone;
using trading_engine::feed::SourceId;
using trading_engine::feed::WebSocketClient;
using trading_engine::feed::make_raw_packet;
using trading_engine::feed::to_decode_input_view;
using trading_engine::state::MarketStateStore;
using trading_engine::state::MarketStateView;
using trading_engine::state::from_classified_fill;
using trading_engine::state::from_normalized_batch;

constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;

struct Config {
    std::uint64_t seconds{300};
    std::string asset_id;
    std::string market_id{"live-market"};
    std::string polymarket_endpoint{
        "wss://ws-subscriptions-clob.polymarket.com/ws/market"
    };
    std::string polygon_ws_url;
    std::string contract_address;
};

struct Stats {
    std::atomic<std::uint64_t> ws_packets{0};
    std::atomic<std::uint64_t> normalized_events{0};
    std::atomic<std::uint64_t> filtered_events{0};
    std::atomic<std::uint64_t> book_snapshots{0};
    std::atomic<std::uint64_t> book_deltas{0};
    std::atomic<std::uint64_t> chain_logs{0};
    std::atomic<std::uint64_t> classified_fills{0};
    std::atomic<std::uint64_t> buy_aggressor_fills{0};
    std::atomic<std::uint64_t> sell_aggressor_fills{0};
    std::atomic<std::uint64_t> unknown_fills{0};
    std::atomic<std::uint64_t> removed_fills{0};
    std::atomic<std::uint64_t> snapshots_published{0};
    std::atomic<std::uint64_t> usable_for_depth_count{0};
    std::atomic<std::uint64_t> usable_for_signal_count{0};
    std::atomic<std::uint64_t> state_errors{0};
    std::atomic<std::uint64_t> decode_errors{0};
    std::atomic<std::uint64_t> chain_decode_errors{0};
    std::atomic<std::uint64_t> transport_errors{0};
    std::atomic<std::uint64_t> ping_sent{0};
    std::atomic<std::uint64_t> pong_received{0};
};

std::uint64_t now_ns() {
    const auto now = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count()
    );
}

std::uint64_t checked_sub(
    std::uint64_t end_ns,
    std::uint64_t start_ns
) noexcept {
    return end_ns >= start_ns ? end_ns - start_ns : 0;
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

std::string market_subscription(const std::string& asset_id) {
    return
        std::string{R"({"assets_ids":[")"} +
        escape_json(asset_id) +
        R"("],"type":"market","custom_feature_enabled":true})";
}

std::string order_filled_subscription(
    const std::string& contract_address
) {
    std::string filter{"{\"topics\":[\""};
    filter += OrderFilledDecoder::kOrderFilledTopic0;
    filter += "\"]";
    if (!contract_address.empty()) {
        filter += ",\"address\":\"" + escape_json(contract_address) + "\"";
    }
    filter += "}";

    return
        std::string{R"({"jsonrpc":"2.0","id":1,"method":"eth_subscribe","params":["logs",)"} +
        filter +
        "]}";
}

std::uint64_t parse_hex_u64(const std::string& value) {
    std::size_t offset = 0;
    if (value.size() > 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        offset = 2;
    }

    std::uint64_t out = 0;
    for (; offset < value.size(); ++offset) {
        const char c = value[offset];
        out <<= 4;
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

bool is_pong(const std::string& payload) {
    return payload == "PONG" || payload == "pong" ||
           payload == "\"PONG\"" || payload == "\"pong\"";
}

bool targets_other_asset(
    const trading_engine::decode::NormalizedEvent& event,
    const std::string& asset_id
) {
    if (event.event_type == NormalizedEventType::Heartbeat) {
        return false;
    }

    const std::string& target = !event.asset_id.empty()
        ? event.asset_id
        : event.entity_id;

    return !target.empty() && target != asset_id;
}

Config parse_args(int argc, char** argv) {
    Config config;
    if (const char* asset = std::getenv("POLYMARKET_ASSET_ID")) {
        config.asset_id = asset;
    }
    if (const char* ws = std::getenv("POLYGON_RPC_WS_URL")) {
        config.polygon_ws_url = ws;
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
        } else if (arg == "--asset-id") {
            config.asset_id = value("--asset-id");
        } else if (arg == "--market-id") {
            config.market_id = value("--market-id");
        } else if (arg == "--polygon-ws-url") {
            config.polygon_ws_url = value("--polygon-ws-url");
        } else if (arg == "--contract-address") {
            config.contract_address = value("--contract-address");
        } else if (arg == "--polymarket-endpoint") {
            config.polymarket_endpoint = value("--polymarket-endpoint");
        } else if (arg == "--help" || arg == "-h") {
            throw std::runtime_error(
                "usage: run_market_state_live_smoke --seconds N "
                "--asset-id ASSET_ID [--market-id ID] "
                "[--contract-address ADDRESS]"
            );
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (config.seconds == 0) {
        throw std::runtime_error("--seconds must be greater than zero");
    }
    if (config.asset_id.empty()) {
        throw std::runtime_error(
            "--asset-id is required unless POLYMARKET_ASSET_ID is set"
        );
    }
    if (config.polygon_ws_url.empty()) {
        throw std::runtime_error(
            "POLYGON_RPC_WS_URL is required for chain stream validation"
        );
    }
    return config;
}

void print_report(
    const Config& config,
    const Stats& stats,
    std::uint64_t start_ns,
    std::uint64_t end_ns
) {
    std::cout << "market_state_live_smoke:\n";
    std::cout << "  runtime_seconds: "
              << checked_sub(end_ns, start_ns) / kNsPerSecond << '\n';
    std::cout << "  ws_packets: " << stats.ws_packets.load() << '\n';
    std::cout << "  normalized_events: "
              << stats.normalized_events.load() << '\n';
    std::cout << "  filtered_events: "
              << stats.filtered_events.load() << '\n';
    std::cout << "  book_snapshots: " << stats.book_snapshots.load() << '\n';
    std::cout << "  book_deltas: " << stats.book_deltas.load() << '\n';
    std::cout << "  chain_logs: " << stats.chain_logs.load() << '\n';
    std::cout << "  classified_fills: "
              << stats.classified_fills.load() << '\n';
    std::cout << "  buy_aggressor_fills: "
              << stats.buy_aggressor_fills.load() << '\n';
    std::cout << "  sell_aggressor_fills: "
              << stats.sell_aggressor_fills.load() << '\n';
    std::cout << "  unknown_fills: " << stats.unknown_fills.load() << '\n';
    std::cout << "  removed_fills: " << stats.removed_fills.load() << '\n';
    std::cout << "  snapshots_published: "
              << stats.snapshots_published.load() << '\n';
    std::cout << "  usable_for_depth_count: "
              << stats.usable_for_depth_count.load() << '\n';
    std::cout << "  usable_for_signal_count: "
              << stats.usable_for_signal_count.load() << '\n';
    std::cout << "  state_errors: " << stats.state_errors.load() << '\n';
    std::cout << "  decode_errors: " << stats.decode_errors.load() << '\n';
    std::cout << "  chain_decode_errors: "
              << stats.chain_decode_errors.load() << '\n';
    std::cout << "  transport_errors: "
              << stats.transport_errors.load() << '\n';
    std::cout << "  ping_sent: " << stats.ping_sent.load() << '\n';
    std::cout << "  pong_received: " << stats.pong_received.load() << '\n';
    std::cout << "  asset_id: " << config.asset_id << '\n';
}

int run(const Config& config) {
    DecodePipeline pipeline;
    MarketStateStore store;
    MarketStateView view(store);
    OrderFilledDecoder order_decoder;
    Stats stats;
    std::mutex state_mutex;
    std::atomic<bool> fatal{false};
    std::atomic<std::uint64_t> next_packet_id{1};

    WebSocketClient market_ws(config.polymarket_endpoint);
    WebSocketClient polygon_ws(config.polygon_ws_url);

    market_ws.set_on_open([&]() {
        market_ws.send(market_subscription(config.asset_id));
    });
    market_ws.set_on_message([&](const std::string& payload) {
        if (is_pong(payload)) {
            stats.pong_received.fetch_add(1);
        }

        const auto packet = make_raw_packet(
            SourceId::PolymarketMarket,
            1,
            next_packet_id.fetch_add(1),
            payload,
            Codec::None,
            is_pong(payload)
                ? static_cast<std::uint32_t>(PacketHeartbeat)
                : static_cast<std::uint32_t>(PacketNone)
        );

        NormalizedEventBatch batch;
        const auto decoded = pipeline.decode(
            to_decode_input_view(packet),
            &batch
        );

        stats.ws_packets.fetch_add(1);
        stats.normalized_events.fetch_add(
            static_cast<std::uint64_t>(batch.size())
        );
        if (!decoded.ok() &&
            decoded.payload_kind != JsonDecodeKind::NonJsonControl) {
            stats.decode_errors.fetch_add(1);
        }

        NormalizedEventBatch asset_batch;
        for (const auto& event : batch.events) {
            if (targets_other_asset(event, config.asset_id)) {
                stats.filtered_events.fetch_add(1);
                continue;
            }

            if (event.event_type == NormalizedEventType::Snapshot) {
                stats.book_snapshots.fetch_add(1);
            } else if (event.event_type == NormalizedEventType::Delta) {
                stats.book_deltas.fetch_add(1);
            }

            static_cast<void>(asset_batch.push_back(event));
        }

        std::lock_guard<std::mutex> lock(state_mutex);
        bool published = false;
        for (const auto& event : from_normalized_batch(asset_batch)) {
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
            const auto snapshot = view.get_snapshot(config.asset_id);
            if (snapshot.ok && snapshot.value.usable_for_depth) {
                stats.usable_for_depth_count.fetch_add(1);
            }
            if (snapshot.ok && snapshot.value.usable_for_signal) {
                stats.usable_for_signal_count.fetch_add(1);
            }
        }
    });
    market_ws.set_on_error([&](const std::string&) {
        stats.transport_errors.fetch_add(1);
    });

    polygon_ws.set_on_open([&]() {
        polygon_ws.send(order_filled_subscription(config.contract_address));
    });
    polygon_ws.set_on_message([&](const std::string& payload) {
        boost::json::error_code error;
        const auto parsed = json::parse(payload, error);
        if (error || !parsed.is_object()) {
            stats.chain_decode_errors.fetch_add(1);
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
            stats.chain_logs.fetch_add(1);

            const auto decoded = order_decoder.decode_confirmed_fill(
                log,
                config.market_id,
                now_ns()
            );
            if (!decoded.ok) {
                stats.chain_decode_errors.fetch_add(1);
                return;
            }

            if (decoded.fill.asset_id != config.asset_id) {
                return;
            }

            auto classified =
                trading_engine::chain_confirm::classify_confirmed_fill(
                    decoded.fill,
                    log.block_number
                );
            stats.classified_fills.fetch_add(1);
            if (classified.removed) {
                stats.removed_fills.fetch_add(1);
            } else if (
                classified.direction == ConfirmedDirection::BuyAggressor) {
                stats.buy_aggressor_fills.fetch_add(1);
            } else if (
                classified.direction == ConfirmedDirection::SellAggressor) {
                stats.sell_aggressor_fills.fetch_add(1);
            } else {
                stats.unknown_fills.fetch_add(1);
            }

            std::lock_guard<std::mutex> lock(state_mutex);
            const auto result = store.apply(from_classified_fill(classified));
            if (!result.ok()) {
                stats.state_errors.fetch_add(1);
            }
            if (result.snapshot_published) {
                stats.snapshots_published.fetch_add(1);
            }
        } catch (...) {
            stats.chain_decode_errors.fetch_add(1);
        }
    });
    polygon_ws.set_on_error([&](const std::string&) {
        stats.transport_errors.fetch_add(1);
    });

    const auto start_ns = now_ns();
    market_ws.connect();
    polygon_ws.connect();

    std::thread market_thread([&]() {
        try {
            market_ws.run();
        } catch (...) {
            stats.transport_errors.fetch_add(1);
            fatal.store(true);
        }
    });
    std::thread polygon_thread([&]() {
        try {
            polygon_ws.run();
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
            next_ping_ns = now + 10 * kNsPerSecond;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    market_ws.disconnect();
    polygon_ws.disconnect();
    if (market_thread.joinable()) {
        market_thread.join();
    }
    if (polygon_thread.joinable()) {
        polygon_thread.join();
    }

    const auto end_ns = now_ns();
    print_report(config, stats, start_ns, end_ns);

    const bool passed =
        stats.ws_packets.load() > 0 &&
        stats.snapshots_published.load() > 0 &&
        stats.usable_for_depth_count.load() > 0 &&
        stats.decode_errors.load() == 0 &&
        stats.state_errors.load() == 0 &&
        !fatal.load();
    return passed ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_args(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "run_market_state_live_smoke failed: "
                  << error.what() << '\n';
        return 1;
    }
}
