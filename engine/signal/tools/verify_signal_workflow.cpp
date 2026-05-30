#include "engine/signal/core/SignalEngine.h"
#include "engine/signal/edge/LatencyBufferModel.h"
#include "engine/signal/edge/TheoreticalEdgeCalculator.h"
#include "engine/signal/pricing/FeeModel.h"
#include "engine/signal/pricing/VWAPPrecheck.h"
#include "engine/signal/publish/CapturingIntentPublisher.h"
#include "engine/signal/publish/JsonlIntentWriter.h"
#include "engine/signal/rank/OpportunityRanker.h"
#include "engine/signal/reader/FixtureMarketSnapshotReader.h"
#include "engine/signal/reader/OracleArtifactReader.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace trading_engine::signal {
namespace {

namespace json = boost::json;

struct Options {
    std::filesystem::path state_snapshot_path;
    std::filesystem::path oracle_artifact_path;
    std::filesystem::path out_path;
    bool check_determinism = false;
    bool emit_rejections = true;
};

struct PublishedIntentStats {
    std::uint64_t paper_intents = 0;

    std::int64_t bundle_qty_min = 0;
    std::int64_t bundle_qty_max = 0;

    std::int64_t unit_edge_min = 0;
    std::int64_t unit_edge_max = 0;
    std::int64_t total_edge_min = 0;
    std::int64_t total_edge_max = 0;
    std::int64_t edge_bps_min = 0;
    std::int64_t edge_bps_max = 0;

    std::uint64_t intents_with_expiry = 0;
    std::uint64_t intents_with_idempotency_key = 0;
};

struct WorkflowSummary {
    std::uint64_t snapshots_loaded = 0;
    std::uint64_t candidate_bundles_loaded = 0;
    SignalRunResult result;
    PublishedIntentStats published_stats;
    bool settlement_masks_available = false;
    bool determinism_passed = true;
    std::uint64_t second_output_hash = 0;
    std::vector<std::string> errors;

    [[nodiscard]] std::uint64_t vwap_checked() const noexcept {
        return result.vwap_checked;
    }

    [[nodiscard]] std::uint64_t enough_depth() const noexcept {
        return result.paper_opportunities + result.rejected_low_edge;
    }

    [[nodiscard]] std::uint64_t edge_computed() const noexcept {
        return result.edge_computed;
    }

    [[nodiscard]] std::uint64_t consistency_checked() const noexcept {
        return result.bundles_scanned > result.rejected_invalid_settlement
            ? result.bundles_scanned - result.rejected_invalid_settlement
            : 0;
    }

    [[nodiscard]] bool acceptance_ok(bool emit_rejections) const noexcept {
        return errors.empty() &&
               candidate_bundles_loaded > 0 &&
               result.bundles_scanned > 0 &&
               determinism_passed &&
               (!emit_rejections || result.intents_published > 0);
    }
};

void update_minmax(
    std::int64_t value,
    std::int64_t* min_value,
    std::int64_t* max_value,
    bool first
) noexcept {
    if (first) {
        *min_value = value;
        *max_value = value;
        return;
    }
    *min_value = std::min(*min_value, value);
    *max_value = std::max(*max_value, value);
}

[[nodiscard]] PublishedIntentStats summarize_published_intents(
    const std::vector<OpportunityIntent>& intents
) {
    PublishedIntentStats stats;
    bool first_paper = true;

    for (const auto& intent : intents) {
        if (intent.expires_at_ns != 0) {
            ++stats.intents_with_expiry;
        }
        if (!intent.idempotency_key.empty()) {
            ++stats.intents_with_idempotency_key;
        }

        if (intent.status != IntentStatus::PaperOpportunity) {
            continue;
        }

        ++stats.paper_intents;
        update_minmax(
            intent.bundle_qty,
            &stats.bundle_qty_min,
            &stats.bundle_qty_max,
            first_paper
        );
        update_minmax(
            intent.unit_edge_tick,
            &stats.unit_edge_min,
            &stats.unit_edge_max,
            first_paper
        );
        update_minmax(
            intent.total_edge_tick,
            &stats.total_edge_min,
            &stats.total_edge_max,
            first_paper
        );
        update_minmax(
            intent.edge_bps,
            &stats.edge_bps_min,
            &stats.edge_bps_max,
            first_paper
        );
        first_paper = false;
    }

    return stats;
}

[[nodiscard]] std::optional<std::string> require_value(
    int argc,
    char** argv,
    int* index,
    const std::string& name,
    std::vector<std::string>* errors
) {
    if (*index + 1 >= argc) {
        errors->push_back("missing value for " + name);
        return std::nullopt;
    }
    ++(*index);
    return std::string{argv[*index]};
}

[[nodiscard]] bool parse_bool(
    const std::string& value,
    bool* out
) noexcept {
    if (value == "true" || value == "1" || value == "yes") {
        *out = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "no") {
        *out = false;
        return true;
    }
    return false;
}

[[nodiscard]] std::optional<Options> parse_args(
    int argc,
    char** argv,
    std::vector<std::string>* errors
) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--state-snapshot") {
            if (auto value = require_value(argc, argv, &i, arg, errors)) {
                options.state_snapshot_path = *value;
            }
        } else if (arg == "--oracle-artifact") {
            if (auto value = require_value(argc, argv, &i, arg, errors)) {
                options.oracle_artifact_path = *value;
            }
        } else if (arg == "--check-determinism") {
            options.check_determinism = true;
        } else if (arg == "--emit-rejections") {
            if (auto value = require_value(argc, argv, &i, arg, errors)) {
                if (!parse_bool(*value, &options.emit_rejections)) {
                    errors->push_back("invalid bool for --emit-rejections");
                }
            }
        } else if (arg == "--out") {
            if (auto value = require_value(argc, argv, &i, arg, errors)) {
                options.out_path = *value;
            }
        } else {
            errors->push_back("unknown argument: " + arg);
        }
    }

    if (options.state_snapshot_path.empty()) {
        errors->push_back("missing --state-snapshot");
    }
    if (options.oracle_artifact_path.empty()) {
        errors->push_back("missing --oracle-artifact");
    }

    if (!errors->empty()) {
        return std::nullopt;
    }
    return options;
}

[[nodiscard]] std::uint64_t count_snapshots(
    const std::filesystem::path& path
) {
    std::ifstream input(path);
    if (!input) {
        return 0;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    boost::json::error_code error;
    const auto parsed = json::parse(buffer.str(), error);
    if (error || !parsed.is_object()) {
        return 0;
    }
    const auto& object = parsed.as_object();
    const auto it = object.find("snapshots");
    if (it == object.end() || !it->value().is_array()) {
        return 0;
    }
    return it->value().as_array().size();
}

[[nodiscard]] SignalRunResult run_signal_once(
    const Options& options,
    FixtureMarketSnapshotReader* snapshot_reader,
    OracleArtifactReader* artifact_reader,
    CapturingIntentPublisher* publisher
) {
    SignalConfig config;
    config.emit_rejections = options.emit_rejections;

    SettlementMaskChecker settlement_checker;
    VWAPPrecheck vwap;
    FeeModel fee_model(config.default_fee_tick);
    LatencyBufferModel latency_model(config.default_latency_buffer_tick);
    TheoreticalEdgeCalculator edge_calculator(fee_model, latency_model);
    OpportunityRanker ranker;

    SignalEngine engine(
        config,
        snapshot_reader,
        artifact_reader,
        &settlement_checker,
        &vwap,
        &edge_calculator,
        &ranker,
        publisher
    );

    SignalScanContext context;
    context.scan_id = 1;
    context.now_monotonic_ns = 1;
    context.settlement_masks_available = false;
    return engine.scan_once(context);
}

bool write_intents_jsonl(
    const std::filesystem::path& path,
    const std::vector<OpportunityIntent>& intents,
    std::string* error
) {
    if (path.empty()) {
        return true;
    }
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path);
    if (!output) {
        if (error) {
            *error = "failed to open output: " + path.string();
        }
        return false;
    }
    JsonlIntentWriter writer(&output);
    for (const auto& intent : intents) {
        if (!writer.write(intent)) {
            if (error) {
                *error = "failed to write intent";
            }
            return false;
        }
    }
    return true;
}

[[nodiscard]] WorkflowSummary run_workflow(const Options& options) {
    WorkflowSummary summary;
    summary.snapshots_loaded = count_snapshots(options.state_snapshot_path);

    FixtureMarketSnapshotReader snapshot_reader;
    std::string error;
    if (!snapshot_reader.load(options.state_snapshot_path, &error)) {
        summary.errors.push_back(error);
        return summary;
    }

    OracleArtifactReader artifact_reader;
    const auto load = artifact_reader.load(options.oracle_artifact_path);
    if (!load.ok) {
        summary.errors.push_back(load.error);
        return summary;
    }
    summary.candidate_bundles_loaded = artifact_reader.active_bundles().size();

    CapturingIntentPublisher publisher;
    summary.result = run_signal_once(
        options,
        &snapshot_reader,
        &artifact_reader,
        &publisher
    );
    summary.published_stats = summarize_published_intents(
        publisher.intents()
    );

    if (options.check_determinism) {
        FixtureMarketSnapshotReader second_snapshot_reader;
        if (!second_snapshot_reader.load(options.state_snapshot_path, &error)) {
            summary.errors.push_back(error);
            summary.determinism_passed = false;
            return summary;
        }
        OracleArtifactReader second_artifact_reader;
        const auto second_load =
            second_artifact_reader.load(options.oracle_artifact_path);
        if (!second_load.ok) {
            summary.errors.push_back(second_load.error);
            summary.determinism_passed = false;
            return summary;
        }
        CapturingIntentPublisher second_publisher;
        const auto second_result = run_signal_once(
            options,
            &second_snapshot_reader,
            &second_artifact_reader,
            &second_publisher
        );
        summary.second_output_hash = second_result.output_hash;
        summary.determinism_passed =
            summary.result.output_hash == second_result.output_hash &&
            summary.result.bundles_scanned == second_result.bundles_scanned &&
            summary.result.intents_published == second_result.intents_published &&
            summary.result.rejected_rate_limited == second_result.rejected_rate_limited &&
            summary.result.rejected_duplicate == second_result.rejected_duplicate;
    }

    if (!options.out_path.empty()) {
        if (!write_intents_jsonl(
                options.out_path,
                publisher.intents(),
                &error
            )) {
            summary.errors.push_back(error);
        }
    }

    return summary;
}

void print_summary(const WorkflowSummary& summary) {
    std::cout << "signal_workflow:\n";
    std::cout << "  snapshots_loaded: " << summary.snapshots_loaded << '\n';
    std::cout << "  candidate_bundles_loaded: "
              << summary.candidate_bundles_loaded << '\n';
    std::cout << "  candidate_bundles_scanned: "
              << summary.result.bundles_scanned << "\n\n";

    std::cout << "quality_gate:\n";
    std::cout << "  rejected_missing_snapshot: "
              << summary.result.rejected_missing_snapshot << '\n';
    std::cout << "  rejected_bad_state: "
              << summary.result.rejected_bad_market_state << '\n';
    std::cout << "  rejected_stale_snapshot: "
              << summary.result.rejected_stale_snapshot << "\n\n";

    std::cout << "snapshot:\n";
    std::cout << "  consistency_checked: "
              << summary.consistency_checked() << '\n';
    std::cout << "  rejected_snapshot_skew: "
              << summary.result.rejected_snapshot_skew << '\n';
    std::cout << "  rejected_stale_snapshot: "
              << summary.result.rejected_stale_snapshot << "\n\n";

    std::cout << "settlement:\n";
    std::cout << "  settlement_masks_available: "
              << (summary.settlement_masks_available ? "true" : "false")
              << '\n';
    std::cout << "  rejected_invalid_settlement: "
              << summary.result.rejected_invalid_settlement << "\n\n";

    std::cout << "vwap:\n";
    std::cout << "  vwap_checked: " << summary.vwap_checked() << '\n';
    std::cout << "  bundle_qty_min: "
              << summary.published_stats.bundle_qty_min << '\n';
    std::cout << "  bundle_qty_max: "
              << summary.published_stats.bundle_qty_max << '\n';
    std::cout << "  enough_depth: " << summary.enough_depth() << '\n';
    std::cout << "  insufficient_depth: "
              << summary.result.rejected_insufficient_depth << "\n\n";

    std::cout << "edge:\n";
    std::cout << "  edge_computed: " << summary.edge_computed() << '\n';
    std::cout << "  unit_edge_min: "
              << summary.published_stats.unit_edge_min << '\n';
    std::cout << "  unit_edge_max: "
              << summary.published_stats.unit_edge_max << '\n';
    std::cout << "  total_edge_min: "
              << summary.published_stats.total_edge_min << '\n';
    std::cout << "  total_edge_max: "
              << summary.published_stats.total_edge_max << '\n';
    std::cout << "  edge_bps_min: "
              << summary.published_stats.edge_bps_min << '\n';
    std::cout << "  edge_bps_max: "
              << summary.published_stats.edge_bps_max << '\n';
    std::cout << "  above_threshold: "
              << summary.result.paper_opportunities << '\n';
    std::cout << "  below_threshold: "
              << summary.result.rejected_low_edge << "\n\n";

    std::cout << "intents:\n";
    std::cout << "  paper_opportunities: "
              << summary.result.paper_opportunities << '\n';
    std::cout << "  rejected_invalid_settlement: "
              << summary.result.rejected_invalid_settlement << '\n';
    std::cout << "  rejected_bad_market_state: "
              << summary.result.rejected_bad_market_state << '\n';
    std::cout << "  rejected_insufficient_depth: "
              << summary.result.rejected_insufficient_depth << '\n';
    std::cout << "  rejected_low_edge: "
              << summary.result.rejected_low_edge << '\n';
    std::cout << "  duplicate_intents: "
              << summary.result.duplicate_intents << '\n';
    std::cout << "  rejected_duplicate: "
              << summary.result.rejected_duplicate << '\n';
    std::cout << "  rate_limited: "
              << summary.result.rate_limited << '\n';
    std::cout << "  rejected_rate_limited: "
              << summary.result.rejected_rate_limited << '\n';
    std::cout << "  intents_published: "
              << summary.result.intents_published << "\n\n";

    std::cout << "intent_lifecycle:\n";
    std::cout << "  intents_with_expiry: "
              << summary.published_stats.intents_with_expiry << '\n';
    std::cout << "  intents_with_idempotency_key: "
              << summary.published_stats.intents_with_idempotency_key << '\n';
    std::cout << "  duplicate_rejected: "
              << summary.result.rejected_duplicate << '\n';
    std::cout << "  rate_limited: "
              << summary.result.rejected_rate_limited << "\n\n";

    const auto& metrics = summary.result.metrics;
    std::cout << "metrics:\n";
    std::cout << "  signal.scan.count: "
              << metrics.scan_count << '\n';
    std::cout << "  signal.bundle.scanned: "
              << metrics.bundle_scanned << '\n';
    std::cout << "  signal.bundle.rejected: "
              << metrics.bundle_rejected << '\n';
    std::cout << "  signal.bundle.passed: "
              << metrics.bundle_passed << '\n';
    std::cout << "  signal.reject.settled: "
              << metrics.reject_settled << '\n';
    std::cout << "  signal.reject.missing_snapshot: "
              << metrics.reject_missing_snapshot << '\n';
    std::cout << "  signal.reject.stale_lob: "
              << metrics.reject_stale_lob << '\n';
    std::cout << "  signal.reject.snapshot_skew: "
              << metrics.reject_snapshot_skew << '\n';
    std::cout << "  signal.reject.insufficient_depth: "
              << metrics.reject_insufficient_depth << '\n';
    std::cout << "  signal.reject.edge_below_threshold: "
              << metrics.reject_edge_below_threshold << '\n';
    std::cout << "  signal.reject.duplicate: "
              << metrics.reject_duplicate << '\n';
    std::cout << "  signal.reject.rate_limited: "
              << metrics.reject_rate_limited << '\n';
    std::cout << "  signal.intent.published: "
              << metrics.intent_published << '\n';
    std::cout << "  signal.scan.latency_ns:\n";
    std::cout << "    count: " << metrics.scan_latency_ns.count << '\n';
    std::cout << "    last: " << metrics.scan_latency_ns.last_ns << '\n';
    std::cout << "    min: " << metrics.scan_latency_ns.min_ns << '\n';
    std::cout << "    max: " << metrics.scan_latency_ns.max_ns << "\n\n";

    std::cout << "hashes:\n";
    std::cout << "  signal_output_hash: "
              << summary.result.output_hash << '\n';
    std::cout << "  determinism_passed: "
              << (summary.determinism_passed ? "true" : "false") << '\n';

    if (!summary.errors.empty()) {
        std::cout << "\nerrors:\n";
        for (const auto& error : summary.errors) {
            std::cout << "  - " << error << '\n';
        }
    }
}

int run(int argc, char** argv) {
    std::vector<std::string> errors;
    const auto options = parse_args(argc, argv, &errors);
    if (!options) {
        for (const auto& error : errors) {
            std::cerr << error << '\n';
        }
        return 2;
    }

    auto summary = run_workflow(*options);
    print_summary(summary);
    return summary.acceptance_ok(options->emit_rejections) ? 0 : 1;
}

}  // namespace
}  // namespace trading_engine::signal

int main(int argc, char** argv) {
    return trading_engine::signal::run(argc, argv);
}
