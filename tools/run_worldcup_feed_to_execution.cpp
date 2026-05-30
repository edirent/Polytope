#include "decode/core/DecodePipeline.h"
#include "decode/json/JsonDecodeResult.h"
#include "decode/public/NormalizedEventBatch.h"
#include "engine/execution/adapter/PaperExecutionAdapter.h"
#include "engine/execution/core/ExecutionGateway.h"
#include "engine/execution/publish/JsonlExecutionReportWriter.h"
#include "engine/risk/core/RiskEngine.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/signal/core/SignalEngine.h"
#include "engine/signal/edge/LatencyBufferModel.h"
#include "engine/signal/edge/TheoreticalEdgeCalculator.h"
#include "engine/signal/pricing/FeeModel.h"
#include "engine/signal/pricing/VWAPPrecheck.h"
#include "engine/signal/publish/CapturingIntentPublisher.h"
#include "engine/signal/publish/JsonlIntentWriter.h"
#include "engine/signal/rank/OpportunityRanker.h"
#include "engine/signal/reader/MarketStateViewSnapshotReader.h"
#include "engine/signal/reader/OracleArtifactReader.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogWriter.h"
#include "feed/raw_ingest/RawPacket.h"
#include "feed/source_runtime/WebSocketClient.h"
#include "state/MarketStateView.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateEventFilter.h"
#include "state/core/MarketStateStore.h"
#include "state/core/StateUniverse.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

namespace decode = trading_engine::decode;
namespace execution = trading_engine::execution;
namespace feed = trading_engine::feed;
namespace risk = trading_engine::risk;
namespace signal = trading_engine::signal;
namespace state = trading_engine::state;

constexpr std::uint64_t kNsPerMs = 1'000'000ULL;
constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct Config {
    std::uint64_t seconds = 10'800;
    std::uint64_t scan_interval_ms = 5'000;
    std::uint64_t ping_interval_ms = 10'000;
    std::uint64_t intent_ttl_ms = 5'000;
    std::filesystem::path oracle_artifact{
        "runs/worldcup_30615_full_20260530_062519/"
        "oracle_artifact_30615_top8"
    };
    std::filesystem::path out_dir{"runs/worldcup_feed_to_execute_live"};
    std::string event_id{"30615"};
    std::string endpoint{
        "wss://ws-subscriptions-clob.polymarket.com/ws/market"
    };
    bool emit_rejections = true;
};

struct Stats {
    std::uint64_t assets_subscribed = 0;

    std::atomic<std::uint64_t> feed_messages{0};
    std::atomic<std::uint64_t> feed_bytes{0};
    std::atomic<std::uint64_t> raw_packets_written{0};
    std::atomic<std::uint64_t> normalized_events{0};
    std::atomic<std::uint64_t> book_snapshots{0};
    std::atomic<std::uint64_t> book_deltas{0};
    std::atomic<std::uint64_t> book_snapshots_applied{0};
    std::atomic<std::uint64_t> book_deltas_applied{0};
    std::atomic<std::uint64_t> heartbeats{0};
    std::atomic<std::uint64_t> decode_errors{0};
    std::atomic<std::uint64_t> state_errors{0};
    std::atomic<std::uint64_t> transport_errors{0};
    std::atomic<std::uint64_t> ping_sent{0};
    std::atomic<std::uint64_t> pong_received{0};

    std::atomic<std::uint64_t> state_filter_events_seen{0};
    std::atomic<std::uint64_t> state_filter_events_passed{0};
    std::atomic<std::uint64_t> state_filter_events_filtered{0};
    std::atomic<std::uint64_t> state_filter_filtered_paired_asset{0};
    std::atomic<std::uint64_t> state_filter_filtered_non_universe_asset{0};
    std::atomic<std::uint64_t> state_filter_filtered_non_universe_market{0};
    std::atomic<std::uint64_t> state_filter_filtered_missing_asset{0};

    std::atomic<std::uint64_t> scans{0};
    std::atomic<std::uint64_t> signal_bundles_scanned{0};
    std::atomic<std::uint64_t> signal_intents_published{0};
    std::atomic<std::uint64_t> signal_paper_opportunities{0};
    std::atomic<std::uint64_t> signal_rejected_low_edge{0};
    std::atomic<std::uint64_t> signal_rejected_missing_snapshot{0};
    std::atomic<std::uint64_t> signal_rejected_bad_state{0};
    std::atomic<std::uint64_t> signal_rejected_insufficient_depth{0};

    std::atomic<std::uint64_t> risk_evaluated{0};
    std::atomic<std::uint64_t> risk_approved{0};
    std::atomic<std::uint64_t> risk_rejected{0};

    std::atomic<std::uint64_t> execution_plans_submitted{0};
    std::atomic<std::uint64_t> execution_plans_filled{0};
    std::atomic<std::uint64_t> execution_plans_failed{0};
    std::atomic<std::uint64_t> execution_plans_hedge_required{0};
    std::atomic<std::uint64_t> execution_reports_published{0};
    std::atomic<std::uint64_t> reservation_consumed{0};
    std::atomic<std::uint64_t> reservation_released{0};
    std::atomic<std::uint64_t> reservation_expired{0};
};

struct OutputPaths {
    std::filesystem::path raw_log;
    std::filesystem::path payload_jsonl;
    std::filesystem::path signal_intents;
    std::filesystem::path risk_decisions;
    std::filesystem::path execution_reports;
    std::filesystem::path reservation_dispositions;
};

class FileExecutionReportPublisher final
    : public execution::ExecutionReportPublisher {
public:
    FileExecutionReportPublisher(
        std::ostream* output,
        Stats* stats
    ) : writer_(output), stats_(stats) {}

    void publish(const execution::ExecutionReport& report) override {
        writer_.publish(report);
        if (stats_ != nullptr) {
            stats_->execution_reports_published.fetch_add(1);
        }
    }

private:
    execution::JsonlExecutionReportWriter writer_;
    Stats* stats_ = nullptr;
};

class FileReservationDispositionPublisher final
    : public execution::ReservationDispositionPublisher {
public:
    FileReservationDispositionPublisher(
        std::ostream* output,
        Stats* stats
    ) : output_(output), stats_(stats) {}

    void publish(
        const execution::ReservationDisposition& disposition
    ) override {
        if (output_ != nullptr && *output_) {
            *output_
                << "{\"reservation_id\":\""
                << escape(disposition.reservation_id)
                << "\",\"plan_id\":" << disposition.plan_id
                << ",\"type\":\"" << type_to_string(disposition.type)
                << "\",\"reason\":\"" << escape(disposition.reason)
                << "\"}\n";
        }

        if (stats_ == nullptr) {
            return;
        }
        switch (disposition.type) {
            case execution::ReservationDispositionType::Consume:
                stats_->reservation_consumed.fetch_add(1);
                break;
            case execution::ReservationDispositionType::Release:
                stats_->reservation_released.fetch_add(1);
                break;
            case execution::ReservationDispositionType::Expire:
                stats_->reservation_expired.fetch_add(1);
                break;
            case execution::ReservationDispositionType::None:
                break;
        }
    }

private:
    [[nodiscard]] static const char* type_to_string(
        execution::ReservationDispositionType type
    ) noexcept {
        switch (type) {
            case execution::ReservationDispositionType::None:
                return "None";
            case execution::ReservationDispositionType::Release:
                return "Release";
            case execution::ReservationDispositionType::Consume:
                return "Consume";
            case execution::ReservationDispositionType::Expire:
                return "Expire";
        }
        return "None";
    }

    [[nodiscard]] static std::string escape(std::string_view value) {
        std::string out;
        out.reserve(value.size());
        for (const char ch : value) {
            switch (ch) {
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
                    out.push_back(ch);
                    break;
            }
        }
        return out;
    }

    std::ostream* output_ = nullptr;
    Stats* stats_ = nullptr;
};

[[nodiscard]] std::uint64_t now_ns() {
    const auto now = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count()
    );
}

[[nodiscard]] std::uint64_t checked_sub(
    std::uint64_t end_ns,
    std::uint64_t start_ns
) noexcept {
    return end_ns >= start_ns ? end_ns - start_ns : 0;
}

void mix_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= (value >> shift) & 0xffU;
        *hash *= kFnvPrime;
    }
}

[[nodiscard]] std::string escape_json(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
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
                out.push_back(ch);
                break;
        }
    }
    return out;
}

[[nodiscard]] bool is_pong_payload(const std::string& payload) {
    return payload == "PONG" ||
           payload == "pong" ||
           payload == "\"PONG\"" ||
           payload == "\"pong\"";
}

[[nodiscard]] std::string market_subscription(
    const std::vector<std::string>& asset_ids
) {
    std::string message{R"({"assets_ids":[)"};
    for (std::size_t i = 0; i < asset_ids.size(); ++i) {
        if (i > 0) {
            message += ',';
        }
        message += '"';
        message += escape_json(asset_ids[i]);
        message += '"';
    }
    message += R"(],"type":"market","custom_feature_enabled":true})";
    return message;
}

void prepare_output_dir(const std::filesystem::path& path) {
    std::filesystem::create_directories(path);
}

void remove_if_exists(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

[[nodiscard]] OutputPaths output_paths(const Config& config) {
    return {
        .raw_log = config.out_dir / "feed.raw",
        .payload_jsonl = config.out_dir / "feed_payloads.jsonl",
        .signal_intents = config.out_dir / "signal_intents.jsonl",
        .risk_decisions = config.out_dir / "risk_decisions.jsonl",
        .execution_reports = config.out_dir / "execution_reports.jsonl",
        .reservation_dispositions =
            config.out_dir / "reservation_dispositions.jsonl"
    };
}

[[nodiscard]] Config parse_args(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto require_value = [&](const char* option) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string(option) + " requires value");
            }
            return argv[++i];
        };

        if (arg == "--seconds") {
            config.seconds = std::stoull(require_value("--seconds"));
        } else if (arg == "--scan-interval-ms") {
            config.scan_interval_ms =
                std::stoull(require_value("--scan-interval-ms"));
        } else if (arg == "--intent-ttl-ms") {
            config.intent_ttl_ms =
                std::stoull(require_value("--intent-ttl-ms"));
        } else if (arg == "--oracle-artifact") {
            config.oracle_artifact = require_value("--oracle-artifact");
        } else if (arg == "--out-dir") {
            config.out_dir = require_value("--out-dir");
        } else if (arg == "--event-id") {
            config.event_id = require_value("--event-id");
        } else if (arg == "--endpoint") {
            config.endpoint = require_value("--endpoint");
        } else if (arg == "--emit-rejections") {
            const auto value = require_value("--emit-rejections");
            config.emit_rejections =
                value == "1" || value == "true" || value == "yes";
        } else if (arg == "--help" || arg == "-h") {
            throw std::runtime_error(
                "usage: run_worldcup_feed_to_execution "
                "--oracle-artifact PATH --out-dir DIR --seconds N"
            );
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (config.seconds == 0) {
        throw std::runtime_error("--seconds must be greater than zero");
    }
    if (config.scan_interval_ms == 0) {
        throw std::runtime_error("--scan-interval-ms must be greater than zero");
    }
    if (config.intent_ttl_ms == 0) {
        throw std::runtime_error("--intent-ttl-ms must be greater than zero");
    }
    return config;
}

[[nodiscard]] std::vector<std::string> asset_ids_from_artifact(
    const signal::OracleArtifactReader& reader
) {
    std::set<std::string> unique;
    for (const auto& bundle : reader.active_bundles()) {
        for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
            if (!bundle.legs[i].asset_id.empty()) {
                unique.insert(bundle.legs[i].asset_id);
            }
        }
    }
    return {unique.begin(), unique.end()};
}

[[nodiscard]] state::StateUniverse state_universe_from_artifact(
    const signal::OracleArtifactReader& reader
) {
    state::StateUniverse universe;
    for (const auto& bundle : reader.active_bundles()) {
        for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
            const auto& leg = bundle.legs[i];
            if (!leg.asset_id.empty()) {
                universe.active_asset_ids.insert(leg.asset_id);
            }
            if (!leg.market_id.empty()) {
                universe.active_market_ids.insert(leg.market_id);
            }
        }
    }
    return universe;
}

void update_filter_stats(
    const state::MarketStateEventFilterResult& result,
    Stats* stats
) {
    stats->state_filter_events_seen.fetch_add(1);
    if (result.passed()) {
        stats->state_filter_events_passed.fetch_add(1);
        return;
    }

    stats->state_filter_events_filtered.fetch_add(1);
    switch (result.reason) {
        case state::MarketStateEventFilterReason::PairedAssetNotInUniverse:
            stats->state_filter_filtered_paired_asset.fetch_add(1);
            break;
        case state::MarketStateEventFilterReason::AssetNotInUniverse:
            stats->state_filter_filtered_non_universe_asset.fetch_add(1);
            break;
        case state::MarketStateEventFilterReason::MarketNotInUniverse:
            stats->state_filter_filtered_non_universe_market.fetch_add(1);
            break;
        case state::MarketStateEventFilterReason::MissingAssetId:
            stats->state_filter_filtered_missing_asset.fetch_add(1);
            break;
        case state::MarketStateEventFilterReason::MissingMarketId:
        case state::MarketStateEventFilterReason::None:
            break;
    }
}

void write_risk_decision(
    std::ostream& output,
    const signal::OpportunityIntent& intent,
    const risk::RiskPipelineResult& result
) {
    output
        << "{\"intent_id\":" << intent.intent_id
        << ",\"bundle_id\":" << intent.bundle_id
        << ",\"approved\":" << (result.decision.approved() ? "true" : "false")
        << ",\"reject_reason\":"
        << static_cast<unsigned>(result.decision.reject_reason)
        << ",\"reject_detail\":\""
        << escape_json(result.decision.reject_detail)
        << "\",\"reservation_id\":"
        << result.reservation.reservation_id
        << ",\"risk_total_cost_tick\":"
        << result.cost.risk_total_cost_tick
        << ",\"risk_bundle_qty\":" << result.cost.risk_bundle_qty
        << ",\"output_hash\":" << result.output_hash
        << "}\n";
}

[[nodiscard]] std::vector<state::MarketStateSnapshot> snapshots_for_intent(
    const signal::OpportunityIntent& intent,
    const state::MarketStateView& view
) {
    std::vector<state::MarketStateSnapshot> snapshots;
    std::unordered_set<std::string> seen_assets;
    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto& asset_id = intent.legs[i].asset_id;
        if (asset_id.empty() || !seen_assets.insert(asset_id).second) {
            continue;
        }
        const auto result = view.get_snapshot(asset_id);
        if (result.ok) {
            snapshots.push_back(result.value);
        }
    }
    return snapshots;
}

void update_signal_stats(
    const signal::SignalRunResult& result,
    Stats* stats
) {
    stats->scans.fetch_add(1);
    stats->signal_bundles_scanned.fetch_add(result.bundles_scanned);
    stats->signal_intents_published.fetch_add(result.intents_published);
    stats->signal_paper_opportunities.fetch_add(result.paper_opportunities);
    stats->signal_rejected_low_edge.fetch_add(result.rejected_low_edge);
    stats->signal_rejected_missing_snapshot.fetch_add(
        result.rejected_missing_snapshot
    );
    stats->signal_rejected_bad_state.fetch_add(
        result.rejected_bad_market_state
    );
    stats->signal_rejected_insufficient_depth.fetch_add(
        result.rejected_insufficient_depth
    );
}

void update_execution_stats(
    const execution::ExecutionResult& result,
    Stats* stats
) {
    if (result.plan_id != 0) {
        stats->execution_plans_submitted.fetch_add(1);
    }

    switch (result.status) {
        case execution::PlanStatus::Filled:
            stats->execution_plans_filled.fetch_add(1);
            break;
        case execution::PlanStatus::HedgeRequired:
            stats->execution_plans_hedge_required.fetch_add(1);
            break;
        case execution::PlanStatus::Failed:
        case execution::PlanStatus::Cancelled:
        case execution::PlanStatus::Expired:
            stats->execution_plans_failed.fetch_add(1);
            break;
        default:
            break;
    }
}

void print_summary(
    const Config& config,
    const OutputPaths& paths,
    const Stats& stats,
    std::uint64_t start_ns,
    std::uint64_t end_ns,
    std::uint64_t output_hash
) {
    std::cout << "worldcup_feed_to_execution:\n";
    std::cout << "  event_id: " << config.event_id << '\n';
    std::cout << "  runtime_seconds: "
              << checked_sub(end_ns, start_ns) / kNsPerSecond << '\n';
    std::cout << "  oracle_artifact: "
              << config.oracle_artifact.string() << '\n';
    std::cout << "  assets_subscribed: "
              << stats.assets_subscribed << "\n\n";

    std::cout << "feed:\n";
    std::cout << "  messages: " << stats.feed_messages.load() << '\n';
    std::cout << "  bytes: " << stats.feed_bytes.load() << '\n';
    std::cout << "  raw_packets_written: "
              << stats.raw_packets_written.load() << '\n';
    std::cout << "  normalized_events: "
              << stats.normalized_events.load() << '\n';
    std::cout << "  book_snapshots_seen: "
              << stats.book_snapshots.load() << '\n';
    std::cout << "  book_snapshots_applied: "
              << stats.book_snapshots_applied.load() << '\n';
    std::cout << "  book_deltas_seen: " << stats.book_deltas.load() << '\n';
    std::cout << "  book_deltas_applied: "
              << stats.book_deltas_applied.load() << '\n';
    std::cout << "  heartbeats: " << stats.heartbeats.load() << '\n';
    std::cout << "  decode_errors: " << stats.decode_errors.load() << '\n';
    std::cout << "  state_errors: " << stats.state_errors.load() << '\n';
    std::cout << "  transport_errors: "
              << stats.transport_errors.load() << "\n\n";

    std::cout << "state_filter:\n";
    std::cout << "  events_seen: "
              << stats.state_filter_events_seen.load() << '\n';
    std::cout << "  events_passed: "
              << stats.state_filter_events_passed.load() << '\n';
    std::cout << "  events_filtered: "
              << stats.state_filter_events_filtered.load() << '\n';
    std::cout << "  filtered_paired_asset: "
              << stats.state_filter_filtered_paired_asset.load() << '\n';
    std::cout << "  filtered_non_universe_asset: "
              << stats.state_filter_filtered_non_universe_asset.load() << '\n';
    std::cout << "  filtered_non_universe_market: "
              << stats.state_filter_filtered_non_universe_market.load() << '\n';
    std::cout << "  filtered_missing_asset: "
              << stats.state_filter_filtered_missing_asset.load() << "\n\n";

    std::cout << "signal:\n";
    std::cout << "  scans: " << stats.scans.load() << '\n';
    std::cout << "  bundles_scanned: "
              << stats.signal_bundles_scanned.load() << '\n';
    std::cout << "  intents_published: "
              << stats.signal_intents_published.load() << '\n';
    std::cout << "  paper_opportunities: "
              << stats.signal_paper_opportunities.load() << '\n';
    std::cout << "  rejected_low_edge: "
              << stats.signal_rejected_low_edge.load() << '\n';
    std::cout << "  rejected_missing_snapshot: "
              << stats.signal_rejected_missing_snapshot.load() << '\n';
    std::cout << "  rejected_bad_state: "
              << stats.signal_rejected_bad_state.load() << '\n';
    std::cout << "  rejected_insufficient_depth: "
              << stats.signal_rejected_insufficient_depth.load() << "\n\n";

    std::cout << "risk:\n";
    std::cout << "  evaluated: " << stats.risk_evaluated.load() << '\n';
    std::cout << "  approved: " << stats.risk_approved.load() << '\n';
    std::cout << "  rejected: " << stats.risk_rejected.load() << "\n\n";

    std::cout << "execution:\n";
    std::cout << "  plans_submitted: "
              << stats.execution_plans_submitted.load() << '\n';
    std::cout << "  plans_filled: "
              << stats.execution_plans_filled.load() << '\n';
    std::cout << "  plans_failed: "
              << stats.execution_plans_failed.load() << '\n';
    std::cout << "  plans_hedge_required: "
              << stats.execution_plans_hedge_required.load() << '\n';
    std::cout << "  execution_reports_published: "
              << stats.execution_reports_published.load() << '\n';
    std::cout << "  reservation_consumed: "
              << stats.reservation_consumed.load() << '\n';
    std::cout << "  reservation_released: "
              << stats.reservation_released.load() << '\n';
    std::cout << "  reservation_expired: "
              << stats.reservation_expired.load() << "\n\n";

    std::cout << "outputs:\n";
    std::cout << "  raw_log: " << paths.raw_log.string() << '\n';
    std::cout << "  payload_jsonl: " << paths.payload_jsonl.string() << '\n';
    std::cout << "  signal_intents: "
              << paths.signal_intents.string() << '\n';
    std::cout << "  risk_decisions: "
              << paths.risk_decisions.string() << '\n';
    std::cout << "  execution_reports: "
              << paths.execution_reports.string() << '\n';
    std::cout << "  reservation_dispositions: "
              << paths.reservation_dispositions.string() << "\n\n";

    std::cout << "hashes:\n";
    std::cout << "  output_hash: " << output_hash << '\n';
}

int run(const Config& config) {
    prepare_output_dir(config.out_dir);
    const auto paths = output_paths(config);
    for (const auto& path : {
             paths.raw_log,
             paths.payload_jsonl,
             paths.signal_intents,
             paths.risk_decisions,
             paths.execution_reports,
             paths.reservation_dispositions
         }) {
        remove_if_exists(path);
    }

    signal::OracleArtifactReader artifact_reader;
    const auto load_result = artifact_reader.load(config.oracle_artifact);
    if (!load_result.ok) {
        throw std::runtime_error(
            "failed to load oracle artifact: " + load_result.error
        );
    }
    const auto asset_ids = asset_ids_from_artifact(artifact_reader);
    if (asset_ids.empty()) {
        throw std::runtime_error(
            "oracle artifact has no active bundle assets"
        );
    }
    const auto state_universe = state_universe_from_artifact(artifact_reader);
    const state::MarketStateEventFilter event_filter(state_universe);

    feed::RawLogWriter raw_writer(paths.raw_log.string());
    std::ofstream payload_out(paths.payload_jsonl);
    std::ofstream signal_out(paths.signal_intents);
    std::ofstream risk_out(paths.risk_decisions);
    std::ofstream execution_out(paths.execution_reports);
    std::ofstream disposition_out(paths.reservation_dispositions);
    if (!payload_out || !signal_out || !risk_out ||
        !execution_out || !disposition_out) {
        throw std::runtime_error("failed to open one or more output files");
    }

    decode::DecodePipeline decode_pipeline;
    state::MarketStateStore market_store;
    state::MarketStateView market_view(market_store);
    signal::MarketStateViewSnapshotReader snapshot_reader(market_view);
    signal::SettlementMaskChecker settlement_checker;
    signal::VWAPPrecheck vwap;
    signal::FeeModel fee_model(0);
    signal::LatencyBufferModel latency_model(0);
    signal::TheoreticalEdgeCalculator edge_calculator(
        fee_model,
        latency_model
    );
    signal::OpportunityRanker ranker;
    signal::CapturingIntentPublisher signal_publisher;
    signal::SignalConfig signal_config;
    signal_config.emit_rejections = config.emit_rejections;
    signal_config.max_lob_age_ns = 30 * static_cast<std::int64_t>(kNsPerSecond);
    signal_config.max_snapshot_version_skew = 1'000'000;
    signal_config.max_intents_per_second = 10'000;
    signal::SignalEngine signal_engine(
        signal_config,
        &snapshot_reader,
        &artifact_reader,
        &settlement_checker,
        &vwap,
        &edge_calculator,
        &ranker,
        &signal_publisher
    );
    signal::JsonlIntentWriter signal_writer(&signal_out);

    risk::RiskEngine risk_engine;
    auto risk_policy = risk::with_computed_policy_hash(risk::RiskPolicySnapshot{});
    risk_policy.max_book_age_ns = 30 * static_cast<std::int64_t>(kNsPerSecond);
    risk_policy.max_intent_age_ns = 30 * static_cast<std::int64_t>(kNsPerSecond);
    risk_policy.max_snapshot_skew_ns = 1'000'000;
    risk_policy.max_allowed_cost_drift_tick = 1'000'000'000;
    risk_policy.min_depth_margin_ratio = 1.0;

    execution::PaperExecutionAdapter paper_adapter;
    Stats stats;
    stats.assets_subscribed = asset_ids.size();
    FileExecutionReportPublisher execution_publisher(&execution_out, &stats);
    FileReservationDispositionPublisher disposition_publisher(
        &disposition_out,
        &stats
    );
    execution::ExecutionGateway execution_gateway(
        &paper_adapter,
        &execution_publisher,
        &disposition_publisher
    );
    execution::ExecutionConfig execution_config;
    execution_config.execution_enabled = true;
    execution_config.mode = execution::ExecutionMode::Paper;
    execution_config.paper_mode = execution::PaperExecutionMode::PaperAtomic;
    execution_config.max_child_orders_per_plan = 16;
    execution_config.max_order_age_ns =
        static_cast<std::int64_t>(config.intent_ttl_ms * kNsPerMs);

    std::atomic<bool> fatal{false};
    std::mutex state_mutex;
    std::atomic<std::uint64_t> next_packet_id{1};
    std::uint64_t output_hash = kFnvOffset;

    feed::WebSocketClient client(config.endpoint);
    client.set_on_open([&]() {
        client.send(market_subscription(asset_ids));
    });
    client.set_on_message([&](const std::string& payload) {
        stats.feed_messages.fetch_add(1);
        stats.feed_bytes.fetch_add(static_cast<std::uint64_t>(payload.size()));
        if (is_pong_payload(payload)) {
            stats.pong_received.fetch_add(1);
        }

        std::uint32_t flags = feed::PacketNone;
        if (is_pong_payload(payload)) {
            flags |= static_cast<std::uint32_t>(feed::PacketHeartbeat);
        }

        try {
            auto packet = feed::make_raw_packet(
                feed::SourceId::PolymarketMarket,
                1,
                next_packet_id.fetch_add(1),
                payload,
                feed::Codec::None,
                flags
            );
            raw_writer.write_packet(packet);
            payload_out << packet.payload << '\n';
            stats.raw_packets_written.fetch_add(1);

            decode::NormalizedEventBatch batch;
            const auto decoded = decode_pipeline.decode(
                feed::to_decode_input_view(packet),
                &batch
            );
            if (!decoded.ok() &&
                decoded.payload_kind != decode::JsonDecodeKind::NonJsonControl) {
                stats.decode_errors.fetch_add(1);
            }

            stats.normalized_events.fetch_add(batch.size());

            std::lock_guard<std::mutex> lock(state_mutex);
            for (const auto& event : batch.events) {
                if (event.event_type == decode::NormalizedEventType::Snapshot) {
                    stats.book_snapshots.fetch_add(1);
                } else if (
                    event.event_type == decode::NormalizedEventType::Delta) {
                    stats.book_deltas.fetch_add(1);
                } else if (
                    event.event_type == decode::NormalizedEventType::Heartbeat) {
                    stats.heartbeats.fetch_add(1);
                }
            }
            for (const auto& event : state::from_normalized_batch(batch)) {
                const auto filter_result = event_filter.filter(event);
                update_filter_stats(filter_result, &stats);
                if (!filter_result.passed()) {
                    continue;
                }

                const auto result = market_store.apply(event);
                if (result.ok()) {
                    if (event.type == state::MarketStateEventType::WsBookSnapshot) {
                        stats.book_snapshots_applied.fetch_add(1);
                    } else if (
                        event.type == state::MarketStateEventType::WsBookDelta) {
                        stats.book_deltas_applied.fetch_add(1);
                    }
                }
                if (!result.ok()) {
                    stats.state_errors.fetch_add(1);
                }
            }
        } catch (const std::exception& error) {
            stats.state_errors.fetch_add(1);
            std::cerr << "live workflow message error: "
                      << error.what() << '\n';
        }
    });
    client.set_on_error([&](const std::string& error) {
        stats.transport_errors.fetch_add(1);
        std::cerr << "live workflow transport error: " << error << '\n';
    });

    const auto start_ns = now_ns();
    client.connect();

    std::thread reader_thread([&]() {
        try {
            client.run();
        } catch (const std::exception& error) {
            stats.transport_errors.fetch_add(1);
            fatal.store(true);
            std::cerr << "live workflow reader error: "
                      << error.what() << '\n';
        }
    });

    std::uint64_t next_scan_ns = start_ns + config.scan_interval_ms * kNsPerMs;
    std::uint64_t next_ping_ns = start_ns;
    std::uint64_t scan_id = 1;
    std::size_t written_intents = 0;
    const auto deadline_ns = start_ns + config.seconds * kNsPerSecond;

    while (now_ns() < deadline_ns && !fatal.load()) {
        const auto now = now_ns();
        if (client.connected() && now >= next_ping_ns) {
            client.send("PING");
            stats.ping_sent.fetch_add(1);
            next_ping_ns = now + config.ping_interval_ms * kNsPerMs;
        }

        if (now >= next_scan_ns) {
            signal::SignalRunResult signal_result;
            std::lock_guard<std::mutex> lock(state_mutex);
            signal::SignalScanContext context;
            context.scan_id = scan_id++;
            context.now_monotonic_ns = now;
            context.settlement_masks_available = false;
            signal_result = signal_engine.scan_once(context);
            update_signal_stats(signal_result, &stats);
            mix_u64(&output_hash, signal_result.output_hash);

            const auto& intents = signal_publisher.intents();
            while (written_intents < intents.size()) {
                auto intent = intents[written_intents++];
                if (intent.created_ts_ns == 0) {
                    intent.created_ts_ns = now;
                }
                if (intent.expires_at_ns <= now) {
                    intent.expires_at_ns =
                        now + config.intent_ttl_ms * kNsPerMs;
                }
                if (!signal_writer.write(intent)) {
                    throw std::runtime_error("failed to write signal intent");
                }

                if (intent.status != signal::IntentStatus::PaperOpportunity) {
                    continue;
                }

                auto snapshots = snapshots_for_intent(intent, market_view);

                risk::RiskEvaluationContext risk_context;
                risk_context.now_ns = now;
                risk_context.latest_snapshots = snapshots;
                risk_context.policy = risk_policy;

                const auto risk_result =
                    risk_engine.evaluate(intent, risk_context);
                write_risk_decision(risk_out, intent, risk_result);
                stats.risk_evaluated.fetch_add(1);
                mix_u64(&output_hash, risk_result.output_hash);
                if (!risk_result.decision.approved()) {
                    stats.risk_rejected.fetch_add(1);
                    continue;
                }
                stats.risk_approved.fetch_add(1);

                execution::ApprovedIntentEnvelope envelope;
                envelope.source_intent = intent;
                envelope.approval.decision_id =
                    risk_result.output_hash != 0
                        ? risk_result.output_hash
                        : intent.intent_id;
                envelope.approval.reservation_id =
                    risk_result.reservation.reservation_id;
                envelope.approval.bundle_id = intent.bundle_id;
                envelope.approval.approved_bundle_qty =
                    risk_result.cost.risk_bundle_qty > 0
                        ? risk_result.cost.risk_bundle_qty
                        : intent.bundle_qty;
                envelope.approval.idempotency_key = intent.idempotency_key;

                execution::ExecutionContext execution_context;
                execution_context.now_ns = now;
                execution_context.snapshots = std::move(snapshots);
                execution_context.config = execution_config;

                const auto execution_result =
                    execution_gateway.submit_approved_intent(
                        envelope,
                        execution_context
                    );
                update_execution_stats(execution_result, &stats);
                mix_u64(&output_hash, execution_result.plan_id);
                mix_u64(
                    &output_hash,
                    static_cast<std::uint64_t>(execution_result.status)
                );

                static_cast<void>(execution_gateway.poll());
            }

            signal_out.flush();
            risk_out.flush();
            execution_out.flush();
            disposition_out.flush();
            next_scan_ns = now + config.scan_interval_ms * kNsPerMs;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    client.disconnect();
    if (reader_thread.joinable()) {
        reader_thread.join();
    }

    raw_writer.flush();
    payload_out.flush();
    signal_out.flush();
    risk_out.flush();
    execution_out.flush();
    disposition_out.flush();

    const auto end_ns = now_ns();
    print_summary(config, paths, stats, start_ns, end_ns, output_hash);

    const bool passed =
        stats.feed_messages.load() > 0 &&
        stats.raw_packets_written.load() > 0 &&
        stats.decode_errors.load() == 0 &&
        !fatal.load();
    return passed ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_args(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "run_worldcup_feed_to_execution failed: "
                  << error.what() << '\n';
        return 1;
    }
}
