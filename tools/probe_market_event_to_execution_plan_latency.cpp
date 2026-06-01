#include "decode/core/DecodePipeline.h"
#include "decode/json/JsonDecodeResult.h"
#include "decode/public/NormalizedEventBatch.h"
#include "engine/decision_fastpath/kernel/FixedShapeKernelSpec.h"
#include "engine/execution/adapter/PaperExecutionAdapter.h"
#include "engine/execution/plan/ExecutionPlanner.h"
#include "engine/order_decision/core/OrderDecisionEngine.h"
#include "engine/risk/core/RiskEngine.h"
#include "engine/risk/publish/CapturingRiskPublisher.h"
#include "engine/risk/publish/HashOnlyRiskPublisher.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/signal/core/SignalEngine.h"
#include "engine/signal/edge/LatencyBufferModel.h"
#include "engine/signal/edge/TheoreticalEdgeCalculator.h"
#include "engine/signal/pricing/FeeModel.h"
#include "engine/signal/pricing/VWAPPrecheck.h"
#include "engine/signal/publish/CapturingIntentPublisher.h"
#include "engine/signal/publish/HashOnlySignalPublisher.h"
#include "engine/signal/publish/IIntentPublisher.h"
#include "engine/signal/public/SignalRiskHandoff.h"
#include "engine/signal/rank/OpportunityRanker.h"
#include "engine/signal/reader/MarketStateViewSnapshotReader.h"
#include "engine/signal/reader/OracleArtifactReader.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "feed/raw_ingest/RawPacket.h"
#include "state/MarketStateView.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateEventFilter.h"
#include "state/core/MarketStateStore.h"
#include "state/core/StateUniverse.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace decode = trading_engine::decode;
namespace fast = trading_engine::decision_fastpath;
namespace execution = trading_engine::execution;
namespace feed = trading_engine::feed;
namespace order_decision = trading_engine::order_decision;
namespace oracle = trading_engine::oracle;
namespace risk = trading_engine::risk;
namespace signal = trading_engine::signal;
namespace state = trading_engine::state;

constexpr std::uint64_t kNsPerMs = 1'000'000ULL;
constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

enum class PublisherMode {
    Hash,
    Jsonl,
    Null
};

enum class DecisionPathMode {
    Generic,
    FastKernel
};

struct Config {
    std::filesystem::path raw_path{
        "runs/worldcup_feed_to_execute_30m_20260530_170629/feed.raw"
    };
    std::filesystem::path oracle_artifact{
        "runs/worldcup_30615_full_20260530_062519/"
        "oracle_artifact_30615_top8"
    };
    std::uint64_t repeat = 1;
    std::uint64_t warmup_pass_events = 0;
    std::uint64_t max_plan_samples = 0;
    std::uint64_t intent_ttl_ms = 5'000;
    PublisherMode publisher_mode = PublisherMode::Hash;
    DecisionPathMode decision_path = DecisionPathMode::Generic;
    bool check_determinism = false;
};

[[nodiscard]] const char* to_string(PublisherMode mode) noexcept {
    switch (mode) {
        case PublisherMode::Hash:
            return "hash";
        case PublisherMode::Jsonl:
            return "jsonl";
        case PublisherMode::Null:
            return "null";
    }
    return "hash";
}

[[nodiscard]] const char* to_string(DecisionPathMode mode) noexcept {
    switch (mode) {
        case DecisionPathMode::Generic:
            return "generic";
        case DecisionPathMode::FastKernel:
            return "fast-kernel";
    }
    return "generic";
}

struct LatencyStats {
    std::uint64_t count = 0;
    double mean_us = 0.0;
    double p50_us = 0.0;
    double p90_us = 0.0;
    double p95_us = 0.0;
    double p99_us = 0.0;
    double max_us = 0.0;
};

struct StateApplyBreakdownSamples {
    std::vector<std::uint64_t> total_ns;
    std::vector<std::uint64_t> mutation_ns;
    std::vector<std::uint64_t> make_result_ns;
    std::vector<std::uint64_t> hash_entity_ns;
    std::vector<std::uint64_t> hash_global_ns;
    std::vector<std::uint64_t> snapshot_build_ns;
    std::vector<std::uint64_t> snapshot_publish_ns;
};

struct StateApplyBreakdownByKind {
    StateApplyBreakdownSamples all;
    StateApplyBreakdownSamples snapshot;
    StateApplyBreakdownSamples delta;
    StateApplyBreakdownSamples heartbeat;
    StateApplyBreakdownSamples other;
};

struct LatencySamples {
    std::vector<std::uint64_t> state_apply_all_passed_ns;
    std::vector<std::uint64_t> state_mutation_ex_publish_all_passed_ns;
    std::vector<std::uint64_t> snapshot_publish_all_passed_ns;
    std::vector<std::uint64_t> snapshot_publish_actual_ns;
    std::vector<std::uint64_t> state_apply_snapshot_ns;
    std::vector<std::uint64_t> state_apply_delta_ns;
    std::vector<std::uint64_t> state_apply_heartbeat_ns;
    std::vector<std::uint64_t> state_apply_other_ns;

    std::vector<std::uint64_t> state_apply_ns;
    std::vector<std::uint64_t> signal_scan_ns;
    std::vector<std::uint64_t> signal_bundle_scan_ns;
    std::vector<std::uint64_t> signal_settlement_check_ns;
    std::vector<std::uint64_t> signal_snapshot_reader_ns;
    std::vector<std::uint64_t> signal_snapshot_consistency_guard_ns;
    std::vector<std::uint64_t> signal_price_vector_builder_ns;
    std::vector<std::uint64_t> signal_vwap_precheck_ns;
    std::vector<std::uint64_t> signal_edge_calculator_ns;
    std::vector<std::uint64_t> signal_intent_builder_ns;
    std::vector<std::uint64_t> signal_dedupe_ns;
    std::vector<std::uint64_t> signal_rate_limiter_ns;
    std::vector<std::uint64_t> signal_publisher_ns;
    std::vector<std::uint64_t> risk_eval_ns;
    std::vector<std::uint64_t> risk_total_ns;
    std::vector<std::uint64_t> risk_stage_sum_ns;
    std::vector<std::uint64_t> risk_unattributed_ns;
    std::vector<std::uint64_t> risk_kill_switch_guard_ns;
    std::vector<std::uint64_t> risk_intent_validator_ns;
    std::vector<std::uint64_t> risk_evidence_verifier_ns;
    std::vector<std::uint64_t> risk_expiry_guard_ns;
    std::vector<std::uint64_t> risk_duplicate_guard_ns;
    std::vector<std::uint64_t> risk_rate_limit_guard_ns;
    std::vector<std::uint64_t> risk_market_state_guard_ns;
    std::vector<std::uint64_t> risk_snapshot_freshness_guard_ns;
    std::vector<std::uint64_t> risk_cost_revalidator_ns;
    std::vector<std::uint64_t> risk_vwap_revalidator_ns;
    std::vector<std::uint64_t> risk_edge_guard_ns;
    std::vector<std::uint64_t> risk_max_loss_guard_ns;
    std::vector<std::uint64_t> risk_exposure_guard_ns;
    std::vector<std::uint64_t> risk_inventory_guard_ns;
    std::vector<std::uint64_t> risk_partial_fill_guard_ns;
    std::vector<std::uint64_t> risk_reservation_book_ns;
    std::vector<std::uint64_t> risk_audit_trace_ns;
    std::vector<std::uint64_t> risk_decision_build_ns;
    std::vector<std::uint64_t> risk_publisher_ns;
    std::vector<std::uint64_t> risk_metrics_ns;
    std::vector<std::uint64_t> risk_cheap_guards_ns;
    std::vector<std::uint64_t> risk_snapshot_revalidation_ns;
    std::vector<std::uint64_t> risk_vwap_revalidation_ns;
    std::vector<std::uint64_t> risk_edge_guard_group_ns;
    std::vector<std::uint64_t> risk_exposure_inventory_ns;
    std::vector<std::uint64_t> risk_partial_fill_ns;
    std::vector<std::uint64_t> risk_reservation_ns;
    std::vector<std::uint64_t> risk_audit_publish_ns;
    std::vector<std::uint64_t> order_decision_ns;
    std::vector<std::uint64_t> order_decision_input_validate_ns;
    std::vector<std::uint64_t> order_decision_depth_view_prepare_ns;
    std::vector<std::uint64_t> order_decision_cost_curve_build_ns;
    std::vector<std::uint64_t> order_decision_candidate_enumerate_ns;
    std::vector<std::uint64_t> order_decision_candidate_sort_dedup_ns;
    std::vector<std::uint64_t> order_decision_candidate_eval_ns;
    std::vector<std::uint64_t> order_decision_limit_price_build_ns;
    std::vector<std::uint64_t> order_decision_decision_build_ns;
    std::vector<std::uint64_t> order_decision_decision_hash_ns;
    std::vector<std::uint64_t> order_decision_debug_materialize_ns;
    std::vector<std::uint64_t> order_decision_publisher_ns;
    std::vector<std::uint64_t> order_decision_stage_sum_ns;
    std::vector<std::uint64_t> order_decision_unattributed_ns;
    std::vector<std::uint64_t> order_decision_candidate_count;
    std::vector<std::uint64_t> order_decision_candidates_evaluated;
    std::vector<std::uint64_t> order_decision_vwap_cost_calls;
    std::vector<std::uint64_t> order_decision_depth_levels_scanned;
    std::vector<std::uint64_t> order_decision_avg_depth_levels_scanned_x1000;
    std::vector<std::uint64_t> order_decision_max_depth_levels_scanned;
    std::vector<std::uint64_t> order_decision_linear_cost_calls;
    std::vector<std::uint64_t> order_decision_prefix_cost_calls;
    std::vector<std::uint64_t> order_decision_prefix_linear_mismatch;
    std::vector<std::uint64_t> order_decision_fixed_shape_spec_cache_hits;
    std::vector<std::uint64_t> order_decision_fixed_shape_spec_cache_misses;
    std::vector<std::uint64_t> order_decision_candidate_set_cache_hits;
    std::vector<std::uint64_t> order_decision_candidate_set_cache_misses;
    std::vector<std::uint64_t> order_decision_leg_cost_cache_hits;
    std::vector<std::uint64_t> order_decision_leg_cost_cache_misses;
    std::vector<std::uint64_t> order_decision_decision_memo_cache_hits;
    std::vector<std::uint64_t> order_decision_decision_memo_cache_misses;
    std::vector<std::uint64_t> order_decision_incremental_eval_steps;
    std::vector<std::uint64_t> order_decision_incremental_leg_updates;
    std::vector<std::uint64_t> order_decision_incremental_level_advances;
    std::vector<std::uint64_t> order_decision_rejected_by_unit_edge;
    std::vector<std::uint64_t> order_decision_rejected_by_total_edge;
    std::vector<std::uint64_t> order_decision_rejected_by_bps;
    std::vector<std::uint64_t> plan_build_ns;
    std::vector<std::uint64_t> execution_context_build_ns;
    std::vector<std::uint64_t> execution_submit_fill_ns;
    std::vector<std::uint64_t> plan_to_order_filled_ns;
    std::vector<std::uint64_t> filter_to_order_filled_ns;
    std::vector<std::uint64_t> overall_total_ns;
    std::vector<std::uint64_t> overall_stage_sum_ns;
    std::vector<std::uint64_t> overall_unattributed_ns;
    std::vector<std::uint64_t> filter_to_state_dispatch_ns;
    std::vector<std::uint64_t> state_to_signal_dispatch_ns;
    std::vector<std::uint64_t> signal_to_risk_handoff_build_ns;
    std::vector<std::uint64_t> risk_input_view_build_ns;
    std::vector<std::uint64_t> risk_context_requery_ns;
    std::vector<std::uint64_t> risk_context_copy_ns;
    std::vector<std::uint64_t> risk_to_execution_envelope_build_ns;
    std::vector<std::uint64_t> fast_kernel_ns;
    std::vector<std::uint64_t> fast_depth_read_ns;
    std::vector<std::uint64_t> overall_unattributed_residual_ns;
    std::vector<std::uint64_t> filter_to_plan_ns;
    std::vector<std::uint64_t> filter_to_plan_cycles;
    std::vector<std::uint64_t> pass_event_to_scan_ns;

    StateApplyBreakdownByKind state_apply_breakdown;
};

struct StateApplyProbeCounters {
    std::uint64_t publish_skipped = 0;
    std::uint64_t publish_performed = 0;
    std::uint64_t full_hash_computed = 0;
    std::uint64_t full_hash_skipped = 0;
    std::uint64_t cache_hit = 0;
    std::uint64_t cache_miss = 0;
};

struct StateApplyCountersByKind {
    StateApplyProbeCounters all;
    StateApplyProbeCounters snapshot;
    StateApplyProbeCounters delta;
    StateApplyProbeCounters heartbeat;
    StateApplyProbeCounters other;
};

struct Counters {
    std::uint64_t raw_packets = 0;
    std::uint64_t normalized_events = 0;
    std::uint64_t decode_errors = 0;

    std::uint64_t filter_events_seen = 0;
    std::uint64_t filter_events_passed = 0;
    std::uint64_t filter_events_filtered = 0;
    std::uint64_t state_errors = 0;
    std::uint64_t snapshots_published = 0;
    std::uint64_t heartbeat_snapshots_published = 0;

    std::uint64_t signal_scans = 0;
    std::uint64_t paper_opportunities = 0;
    std::uint64_t signal_rejections = 0;
    std::uint64_t risk_evaluated = 0;
    std::uint64_t risk_approved = 0;
    std::uint64_t risk_rejected = 0;
    std::uint64_t risk_rejected_low_edge = 0;
    std::uint64_t risk_rejected_cost_limit = 0;
    std::uint64_t risk_rejected_bad_state = 0;
    std::uint64_t risk_rejected_stale = 0;
    std::uint64_t risk_rejected_cost_drift = 0;
    std::uint64_t risk_rejected_invalid_intent = 0;
    std::uint64_t risk_rejected_duplicate = 0;
    std::uint64_t risk_rejected_partial_fill = 0;
    std::uint64_t risk_rejected_other = 0;
    std::string first_risk_reject_detail;
    std::uint64_t risk_vwap_reused_signal_cost = 0;
    std::uint64_t risk_vwap_reused_signal_snapshot = 0;
    std::uint64_t risk_vwap_recomputed = 0;
    std::uint64_t risk_snapshot_fast_path = 0;
    std::uint64_t risk_snapshot_requery = 0;
    std::uint64_t order_decisions_created = 0;
    std::uint64_t order_decision_rejected = 0;
    std::uint64_t order_decision_rejected_no_depth = 0;
    std::uint64_t order_decision_rejected_low_edge = 0;
    std::uint64_t order_decision_rejected_invalid_bundle = 0;
    std::uint64_t order_decision_rejected_unsupported_side = 0;
    std::uint64_t order_decision_rejected_risk_budget = 0;
    std::uint64_t order_decision_rejected_partial_fill = 0;
    std::uint64_t order_decision_rejected_expired = 0;
    std::uint64_t order_decision_rejected_price_protection = 0;
    std::uint64_t order_decision_rejected_other = 0;
    std::uint64_t plans_built = 0;
    std::uint64_t plan_build_rejected = 0;
    std::uint64_t execution_submitted = 0;
    std::uint64_t plans_filled = 0;
    std::uint64_t execution_rejected = 0;
    std::uint64_t execution_reports = 0;
    std::uint64_t execution_rejected_missing_snapshot = 0;
    std::uint64_t execution_rejected_insufficient_depth = 0;
    std::uint64_t execution_rejected_expired = 0;
    std::uint64_t execution_rejected_unsupported_side = 0;
    std::uint64_t execution_rejected_other = 0;

    std::uint64_t snapshot_pass_events = 0;
    std::uint64_t delta_pass_events = 0;
    std::uint64_t heartbeat_pass_events = 0;
    std::uint64_t other_pass_events = 0;

    StateApplyCountersByKind state_apply;
};

struct ProbeState {
    LatencySamples samples;
    Counters counters;
    double tsc_cycles_per_ns = 0.0;
    bool tsc_available = false;
    bool determinism_checked = false;
    bool determinism_passed = true;
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
    for (std::uint8_t i = 0; i < 8; ++i) {
        *hash ^= static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
        *hash *= kFnvPrime;
    }
}

void mix_string(std::uint64_t* hash, const std::string& value) noexcept {
    for (const unsigned char c : value) {
        *hash ^= c;
        *hash *= kFnvPrime;
    }
}

[[nodiscard]] std::uint64_t read_tsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int aux = 0;
    return __rdtscp(&aux);
#else
    return 0;
#endif
}

[[nodiscard]] bool tsc_supported() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return true;
#else
    return false;
#endif
}

[[nodiscard]] double calibrate_tsc_cycles_per_ns() {
    if (!tsc_supported()) {
        return 0.0;
    }

    const auto ns_start = now_ns();
    const auto cycles_start = read_tsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto cycles_end = read_tsc();
    const auto ns_end = now_ns();
    const auto elapsed_ns = checked_sub(ns_end, ns_start);
    if (elapsed_ns == 0 || cycles_end <= cycles_start) {
        return 0.0;
    }
    return static_cast<double>(cycles_end - cycles_start) /
           static_cast<double>(elapsed_ns);
}

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

class NullSignalPublisher final : public signal::IIntentPublisher {
public:
    void publish(const signal::OpportunityIntent&) override {}

    [[nodiscard]] bool requires_materialized_strings() const noexcept override {
        return false;
    }
};

class NullRiskPublisher final : public risk::IRiskDecisionPublisher {
public:
    void publish_decision(
        const risk::RiskDecision&,
        const risk::RiskAuditTrace&
    ) override {}
};

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

        if (arg == "--raw") {
            config.raw_path = require_value("--raw");
        } else if (arg == "--artifact") {
            config.oracle_artifact = require_value("--artifact");
        } else if (arg == "--repeat") {
            config.repeat = std::stoull(require_value("--repeat"));
        } else if (arg == "--warmup-pass-events") {
            config.warmup_pass_events =
                std::stoull(require_value("--warmup-pass-events"));
        } else if (arg == "--max-plan-samples") {
            config.max_plan_samples =
                std::stoull(require_value("--max-plan-samples"));
        } else if (arg == "--intent-ttl-ms") {
            config.intent_ttl_ms =
                std::stoull(require_value("--intent-ttl-ms"));
        } else if (arg == "--publisher-mode") {
            const auto value = require_value("--publisher-mode");
            if (value == "hash") {
                config.publisher_mode = PublisherMode::Hash;
            } else if (value == "jsonl") {
                config.publisher_mode = PublisherMode::Jsonl;
            } else if (value == "null") {
                config.publisher_mode = PublisherMode::Null;
            } else {
                throw std::runtime_error(
                    "--publisher-mode must be hash, jsonl, or null"
                );
            }
        } else if (arg == "--decision-path") {
            const auto value = require_value("--decision-path");
            if (value == "generic") {
                config.decision_path = DecisionPathMode::Generic;
            } else if (value == "fast-kernel") {
                config.decision_path = DecisionPathMode::FastKernel;
            } else {
                throw std::runtime_error(
                    "--decision-path must be generic or fast-kernel"
                );
            }
        } else if (arg == "--check-determinism") {
            config.check_determinism = true;
        } else if (arg == "--help" || arg == "-h") {
            throw std::runtime_error(
                "usage: probe_market_event_to_execution_plan_latency "
                "--raw PATH --artifact PATH [--repeat N] "
                "[--warmup-pass-events N] [--max-plan-samples N] "
                "[--publisher-mode hash|jsonl|null] "
                "[--decision-path generic|fast-kernel] "
                "[--check-determinism]"
            );
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (config.repeat == 0) {
        throw std::runtime_error("--repeat must be greater than zero");
    }
    return config;
}

[[nodiscard]] std::vector<feed::RawPacket> load_packets(
    const std::filesystem::path& raw_path
) {
    feed::RawLogReader reader(raw_path.string());
    std::vector<feed::RawPacket> packets;

    while (true) {
        auto raw = reader.next();
        if (raw.eof()) {
            break;
        }
        if (!raw.ok()) {
            fail("raw read failed: " + raw.message);
        }
        packets.push_back(std::move(*raw.packet));
    }

    return packets;
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

[[nodiscard]] LatencyStats summarize(std::vector<std::uint64_t> values) {
    LatencyStats stats;
    stats.count = static_cast<std::uint64_t>(values.size());
    if (values.empty()) {
        return stats;
    }

    std::sort(values.begin(), values.end());
    const long double sum = std::accumulate(
        values.begin(),
        values.end(),
        static_cast<long double>(0.0)
    );
    const auto percentile = [&](std::uint64_t numerator,
                                std::uint64_t denominator) {
        std::uint64_t index =
            (static_cast<std::uint64_t>(values.size()) * numerator +
             denominator - 1) /
            denominator;
        if (index == 0) {
            index = 1;
        }
        if (index > values.size()) {
            index = static_cast<std::uint64_t>(values.size());
        }
        return static_cast<double>(values[static_cast<std::size_t>(index - 1)]) /
               1000.0;
    };

    stats.mean_us = static_cast<double>(sum / values.size()) / 1000.0;
    stats.p50_us = percentile(50, 100);
    stats.p90_us = percentile(90, 100);
    stats.p95_us = percentile(95, 100);
    stats.p99_us = percentile(99, 100);
    stats.max_us = static_cast<double>(values.back()) / 1000.0;
    return stats;
}

void print_stats_with_indent(
    const std::string& indent,
    const char* name,
    const std::vector<std::uint64_t>& values
) {
    const auto stats = summarize(values);
    std::cout << indent << name << ":\n";
    std::cout << indent << "  count: " << stats.count << '\n';
    std::cout << std::fixed << std::setprecision(3);
    std::cout << indent << "  mean_us: " << stats.mean_us << '\n';
    std::cout << indent << "  p50_us: " << stats.p50_us << '\n';
    std::cout << indent << "  p90_us: " << stats.p90_us << '\n';
    std::cout << indent << "  p95_us: " << stats.p95_us << '\n';
    std::cout << indent << "  p99_us: " << stats.p99_us << '\n';
    std::cout << indent << "  max_us: " << stats.max_us << '\n';
    std::cout.unsetf(std::ios::floatfield);
}

void print_stats(const char* name, const std::vector<std::uint64_t>& values) {
    print_stats_with_indent("  ", name, values);
}

[[nodiscard]] std::uint64_t percentile_raw(
    std::vector<std::uint64_t> values,
    std::uint64_t numerator,
    std::uint64_t denominator
) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    std::uint64_t index =
        (static_cast<std::uint64_t>(values.size()) * numerator +
         denominator - 1) /
        denominator;
    if (index == 0) {
        index = 1;
    }
    if (index > values.size()) {
        index = static_cast<std::uint64_t>(values.size());
    }
    return values[static_cast<std::size_t>(index - 1)];
}

void print_count_p50(
    const char* name,
    const std::vector<std::uint64_t>& values
) {
    std::cout << "  " << name << ": "
              << percentile_raw(values, 50, 100) << '\n';
}

void print_scaled_count_p50(
    const char* name,
    const std::vector<std::uint64_t>& values,
    double scale
) {
    const auto raw = percentile_raw(values, 50, 100);
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "  " << name << ": "
              << static_cast<double>(raw) / scale << '\n';
    std::cout.unsetf(std::ios::floatfield);
}

[[nodiscard]] std::uint64_t sum_values(
    const std::vector<std::uint64_t>& values
) noexcept {
    return std::accumulate(
        values.begin(),
        values.end(),
        static_cast<std::uint64_t>(0)
    );
}

void print_acceptance_latency(
    const char* name,
    const std::vector<std::uint64_t>& values
) {
    const auto stats = summarize(values);
    std::cout << "  " << name << ":\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "    p50_us: " << stats.p50_us << '\n';
    std::cout << "    p95_us: " << stats.p95_us << '\n';
    std::cout << "    p99_us: " << stats.p99_us << '\n';
    std::cout.unsetf(std::ios::floatfield);
}

void count_pass_event_type(
    const state::MarketStateEvent& event,
    Counters* counters
) {
    switch (event.type) {
        case state::MarketStateEventType::WsBookSnapshot:
            ++counters->snapshot_pass_events;
            break;
        case state::MarketStateEventType::WsBookDelta:
            ++counters->delta_pass_events;
            break;
        case state::MarketStateEventType::WsHeartbeat:
            ++counters->heartbeat_pass_events;
            break;
        default:
            ++counters->other_pass_events;
            break;
    }
}

[[nodiscard]] StateApplyBreakdownSamples& samples_for_event(
    const state::MarketStateEvent& event,
    StateApplyBreakdownByKind* samples
) {
    switch (event.type) {
        case state::MarketStateEventType::WsBookSnapshot:
            return samples->snapshot;
        case state::MarketStateEventType::WsBookDelta:
            return samples->delta;
        case state::MarketStateEventType::WsHeartbeat:
            return samples->heartbeat;
        default:
            return samples->other;
    }
}

[[nodiscard]] StateApplyProbeCounters& counters_for_event(
    const state::MarketStateEvent& event,
    StateApplyCountersByKind* counters
) {
    switch (event.type) {
        case state::MarketStateEventType::WsBookSnapshot:
            return counters->snapshot;
        case state::MarketStateEventType::WsBookDelta:
            return counters->delta;
        case state::MarketStateEventType::WsHeartbeat:
            return counters->heartbeat;
        default:
            return counters->other;
    }
}

[[nodiscard]] bool same_apply_counter(
    const StateApplyProbeCounters& lhs,
    const StateApplyProbeCounters& rhs
) noexcept {
    return lhs.publish_skipped == rhs.publish_skipped &&
           lhs.publish_performed == rhs.publish_performed &&
           lhs.full_hash_computed == rhs.full_hash_computed &&
           lhs.full_hash_skipped == rhs.full_hash_skipped &&
           lhs.cache_hit == rhs.cache_hit &&
           lhs.cache_miss == rhs.cache_miss;
}

[[nodiscard]] bool same_apply_counters(
    const StateApplyCountersByKind& lhs,
    const StateApplyCountersByKind& rhs
) noexcept {
    return same_apply_counter(lhs.all, rhs.all) &&
           same_apply_counter(lhs.snapshot, rhs.snapshot) &&
           same_apply_counter(lhs.delta, rhs.delta) &&
           same_apply_counter(lhs.heartbeat, rhs.heartbeat) &&
           same_apply_counter(lhs.other, rhs.other);
}

[[nodiscard]] bool same_counters(
    const Counters& lhs,
    const Counters& rhs
) noexcept {
    return lhs.raw_packets == rhs.raw_packets &&
           lhs.normalized_events == rhs.normalized_events &&
           lhs.decode_errors == rhs.decode_errors &&
           lhs.filter_events_seen == rhs.filter_events_seen &&
           lhs.filter_events_passed == rhs.filter_events_passed &&
           lhs.filter_events_filtered == rhs.filter_events_filtered &&
           lhs.state_errors == rhs.state_errors &&
           lhs.snapshots_published == rhs.snapshots_published &&
           lhs.heartbeat_snapshots_published ==
               rhs.heartbeat_snapshots_published &&
           lhs.signal_scans == rhs.signal_scans &&
           lhs.paper_opportunities == rhs.paper_opportunities &&
           lhs.signal_rejections == rhs.signal_rejections &&
           lhs.risk_evaluated == rhs.risk_evaluated &&
           lhs.risk_approved == rhs.risk_approved &&
           lhs.risk_rejected == rhs.risk_rejected &&
           lhs.risk_rejected_low_edge == rhs.risk_rejected_low_edge &&
           lhs.risk_rejected_cost_limit == rhs.risk_rejected_cost_limit &&
           lhs.risk_rejected_bad_state == rhs.risk_rejected_bad_state &&
           lhs.risk_rejected_stale == rhs.risk_rejected_stale &&
           lhs.risk_rejected_cost_drift == rhs.risk_rejected_cost_drift &&
           lhs.risk_rejected_invalid_intent ==
               rhs.risk_rejected_invalid_intent &&
           lhs.risk_rejected_duplicate == rhs.risk_rejected_duplicate &&
           lhs.risk_rejected_partial_fill ==
               rhs.risk_rejected_partial_fill &&
           lhs.risk_rejected_other == rhs.risk_rejected_other &&
           lhs.risk_vwap_reused_signal_cost ==
               rhs.risk_vwap_reused_signal_cost &&
           lhs.risk_vwap_reused_signal_snapshot ==
               rhs.risk_vwap_reused_signal_snapshot &&
           lhs.risk_vwap_recomputed == rhs.risk_vwap_recomputed &&
           lhs.risk_snapshot_fast_path == rhs.risk_snapshot_fast_path &&
           lhs.risk_snapshot_requery == rhs.risk_snapshot_requery &&
           lhs.order_decisions_created == rhs.order_decisions_created &&
           lhs.order_decision_rejected == rhs.order_decision_rejected &&
           lhs.order_decision_rejected_no_depth ==
               rhs.order_decision_rejected_no_depth &&
           lhs.order_decision_rejected_low_edge ==
               rhs.order_decision_rejected_low_edge &&
           lhs.order_decision_rejected_invalid_bundle ==
               rhs.order_decision_rejected_invalid_bundle &&
           lhs.order_decision_rejected_unsupported_side ==
               rhs.order_decision_rejected_unsupported_side &&
           lhs.order_decision_rejected_risk_budget ==
               rhs.order_decision_rejected_risk_budget &&
           lhs.order_decision_rejected_partial_fill ==
               rhs.order_decision_rejected_partial_fill &&
           lhs.order_decision_rejected_expired ==
               rhs.order_decision_rejected_expired &&
           lhs.order_decision_rejected_price_protection ==
               rhs.order_decision_rejected_price_protection &&
           lhs.order_decision_rejected_other ==
               rhs.order_decision_rejected_other &&
           lhs.plans_built == rhs.plans_built &&
           lhs.plan_build_rejected == rhs.plan_build_rejected &&
           lhs.execution_submitted == rhs.execution_submitted &&
           lhs.plans_filled == rhs.plans_filled &&
           lhs.execution_rejected == rhs.execution_rejected &&
           lhs.execution_reports == rhs.execution_reports &&
           lhs.execution_rejected_missing_snapshot ==
               rhs.execution_rejected_missing_snapshot &&
           lhs.execution_rejected_insufficient_depth ==
               rhs.execution_rejected_insufficient_depth &&
           lhs.execution_rejected_expired == rhs.execution_rejected_expired &&
           lhs.execution_rejected_unsupported_side ==
               rhs.execution_rejected_unsupported_side &&
           lhs.execution_rejected_other == rhs.execution_rejected_other &&
           lhs.snapshot_pass_events == rhs.snapshot_pass_events &&
           lhs.delta_pass_events == rhs.delta_pass_events &&
           lhs.heartbeat_pass_events == rhs.heartbeat_pass_events &&
           lhs.other_pass_events == rhs.other_pass_events &&
           same_apply_counters(lhs.state_apply, rhs.state_apply);
}

void append_apply_breakdown(
    StateApplyBreakdownSamples* samples,
    std::uint64_t apply_ns,
    const state::StateApplyResult& result
) {
    const std::uint64_t non_mutation_ns =
        result.make_result_ns +
        result.snapshot_build_ns +
        result.snapshot_publish_ns;
    samples->total_ns.push_back(apply_ns);
    samples->mutation_ns.push_back(
        apply_ns >= non_mutation_ns ? apply_ns - non_mutation_ns : 0
    );
    samples->make_result_ns.push_back(result.make_result_ns);
    samples->hash_entity_ns.push_back(result.hash_entity_ns);
    samples->hash_global_ns.push_back(result.hash_global_ns);
    samples->snapshot_build_ns.push_back(result.snapshot_build_ns);
    samples->snapshot_publish_ns.push_back(result.snapshot_publish_ns);
}

void record_state_apply_counters(
    const state::MarketStateEvent& event,
    const state::StateApplyResult& result,
    Counters* counters
) {
    auto record = [&](StateApplyProbeCounters* out) {
        if (result.snapshot_published) {
            ++out->publish_performed;
        } else {
            ++out->publish_skipped;
        }

        if (result.full_hash_computed) {
            ++out->full_hash_computed;
        } else {
            ++out->full_hash_skipped;
        }

        out->cache_hit += result.hash_cache_hits;
        out->cache_miss += result.hash_cache_misses;
    };

    record(&counters->state_apply.all);
    record(&counters_for_event(event, &counters->state_apply));
}

void record_state_apply_sample(
    const state::MarketStateEvent& event,
    std::uint64_t apply_ns,
    const state::StateApplyResult& result,
    LatencySamples* samples
) {
    const std::uint64_t publish_ns = result.snapshot_publish_ns;
    samples->state_apply_all_passed_ns.push_back(apply_ns);
    samples->snapshot_publish_all_passed_ns.push_back(publish_ns);
    if (result.snapshot_published) {
        samples->snapshot_publish_actual_ns.push_back(publish_ns);
    }
    samples->state_mutation_ex_publish_all_passed_ns.push_back(
        apply_ns >= publish_ns ? apply_ns - publish_ns : 0
    );

    switch (event.type) {
        case state::MarketStateEventType::WsBookSnapshot:
            samples->state_apply_snapshot_ns.push_back(apply_ns);
            break;
        case state::MarketStateEventType::WsBookDelta:
            samples->state_apply_delta_ns.push_back(apply_ns);
            break;
        case state::MarketStateEventType::WsHeartbeat:
            samples->state_apply_heartbeat_ns.push_back(apply_ns);
            break;
        default:
            samples->state_apply_other_ns.push_back(apply_ns);
            break;
    }

    append_apply_breakdown(
        &samples->state_apply_breakdown.all,
        apply_ns,
        result
    );
    append_apply_breakdown(
        &samples_for_event(event, &samples->state_apply_breakdown),
        apply_ns,
        result
    );
}

void record_order_decision_timings(
    const order_decision::OrderDecisionStageTimings& timings,
    LatencySamples* samples
) {
    samples->order_decision_input_validate_ns.push_back(
        timings.input_validate_ns
    );
    samples->order_decision_depth_view_prepare_ns.push_back(
        timings.depth_view_prepare_ns
    );
    samples->order_decision_cost_curve_build_ns.push_back(
        timings.cost_curve_build_ns
    );
    samples->order_decision_candidate_enumerate_ns.push_back(
        timings.candidate_enumerate_ns
    );
    samples->order_decision_candidate_sort_dedup_ns.push_back(
        timings.candidate_sort_dedup_ns
    );
    samples->order_decision_candidate_eval_ns.push_back(
        timings.candidate_eval_ns
    );
    samples->order_decision_limit_price_build_ns.push_back(
        timings.limit_price_build_ns
    );
    samples->order_decision_decision_build_ns.push_back(
        timings.decision_build_ns
    );
    samples->order_decision_decision_hash_ns.push_back(
        timings.decision_hash_ns
    );
    samples->order_decision_debug_materialize_ns.push_back(
        timings.debug_materialize_ns
    );
    samples->order_decision_publisher_ns.push_back(timings.publisher_ns);
    samples->order_decision_stage_sum_ns.push_back(timings.stage_sum_ns);
    samples->order_decision_unattributed_ns.push_back(
        timings.unattributed_ns
    );
}

void record_order_decision_eval_stats(
    const order_decision::OrderDecisionEvalStats& stats,
    LatencySamples* samples
) {
    samples->order_decision_candidate_count.push_back(stats.candidate_count);
    samples->order_decision_candidates_evaluated.push_back(
        stats.candidates_evaluated
    );
    samples->order_decision_vwap_cost_calls.push_back(stats.vwap_cost_calls);
    samples->order_decision_depth_levels_scanned.push_back(
        stats.depth_levels_scanned
    );
    const auto avg_depth_x1000 =
        stats.vwap_cost_calls == 0
            ? 0ULL
            : (static_cast<std::uint64_t>(stats.depth_levels_scanned) *
               1000ULL) /
                  stats.vwap_cost_calls;
    samples->order_decision_avg_depth_levels_scanned_x1000.push_back(
        avg_depth_x1000
    );
    samples->order_decision_max_depth_levels_scanned.push_back(
        stats.max_depth_levels_scanned
    );
    samples->order_decision_linear_cost_calls.push_back(
        stats.linear_cost_calls
    );
    samples->order_decision_prefix_cost_calls.push_back(
        stats.prefix_cost_calls
    );
    samples->order_decision_prefix_linear_mismatch.push_back(
        stats.prefix_linear_mismatch
    );
    samples->order_decision_fixed_shape_spec_cache_hits.push_back(
        stats.fixed_shape_spec_cache_hits
    );
    samples->order_decision_fixed_shape_spec_cache_misses.push_back(
        stats.fixed_shape_spec_cache_misses
    );
    samples->order_decision_candidate_set_cache_hits.push_back(
        stats.candidate_set_cache_hits
    );
    samples->order_decision_candidate_set_cache_misses.push_back(
        stats.candidate_set_cache_misses
    );
    samples->order_decision_leg_cost_cache_hits.push_back(
        stats.leg_cost_cache_hits
    );
    samples->order_decision_leg_cost_cache_misses.push_back(
        stats.leg_cost_cache_misses
    );
    samples->order_decision_decision_memo_cache_hits.push_back(
        stats.decision_memo_cache_hits
    );
    samples->order_decision_decision_memo_cache_misses.push_back(
        stats.decision_memo_cache_misses
    );
    samples->order_decision_incremental_eval_steps.push_back(
        stats.incremental_eval_steps
    );
    samples->order_decision_incremental_leg_updates.push_back(
        stats.incremental_leg_updates
    );
    samples->order_decision_incremental_level_advances.push_back(
        stats.incremental_level_advances
    );
    samples->order_decision_rejected_by_unit_edge.push_back(
        stats.rejected_by_unit_edge
    );
    samples->order_decision_rejected_by_total_edge.push_back(
        stats.rejected_by_total_edge
    );
    samples->order_decision_rejected_by_bps.push_back(
        stats.rejected_by_bps
    );
}

void record_risk_timings(
    const risk::RiskStageTimings& risk_timings,
    LatencySamples* samples
) {
    samples->risk_total_ns.push_back(risk_timings.total_ns);
    samples->risk_stage_sum_ns.push_back(risk_timings.stage_sum_ns);
    samples->risk_unattributed_ns.push_back(risk_timings.unattributed_ns);
    samples->risk_kill_switch_guard_ns.push_back(
        risk_timings.kill_switch_guard_ns
    );
    samples->risk_intent_validator_ns.push_back(
        risk_timings.intent_validator_ns
    );
    samples->risk_evidence_verifier_ns.push_back(
        risk_timings.evidence_verifier_ns
    );
    samples->risk_expiry_guard_ns.push_back(risk_timings.expiry_guard_ns);
    samples->risk_duplicate_guard_ns.push_back(
        risk_timings.duplicate_guard_ns
    );
    samples->risk_rate_limit_guard_ns.push_back(
        risk_timings.rate_limit_guard_ns
    );
    samples->risk_market_state_guard_ns.push_back(
        risk_timings.market_state_guard_ns
    );
    samples->risk_snapshot_freshness_guard_ns.push_back(
        risk_timings.snapshot_freshness_guard_ns
    );
    samples->risk_cost_revalidator_ns.push_back(
        risk_timings.cost_revalidator_ns
    );
    samples->risk_vwap_revalidator_ns.push_back(
        risk_timings.vwap_revalidator_ns
    );
    samples->risk_edge_guard_ns.push_back(risk_timings.edge_guard_ns);
    samples->risk_max_loss_guard_ns.push_back(risk_timings.max_loss_guard_ns);
    samples->risk_exposure_guard_ns.push_back(risk_timings.exposure_guard_ns);
    samples->risk_inventory_guard_ns.push_back(risk_timings.inventory_guard_ns);
    samples->risk_partial_fill_guard_ns.push_back(
        risk_timings.partial_fill_guard_ns
    );
    samples->risk_reservation_book_ns.push_back(
        risk_timings.reservation_book_ns
    );
    samples->risk_audit_trace_ns.push_back(risk_timings.audit_trace_ns);
    samples->risk_decision_build_ns.push_back(
        risk_timings.risk_decision_build_ns
    );
    samples->risk_publisher_ns.push_back(risk_timings.publisher_ns);
    samples->risk_metrics_ns.push_back(risk_timings.metrics_ns);
    samples->risk_cheap_guards_ns.push_back(
        risk_timings.kill_switch_guard_ns +
        risk_timings.intent_validator_ns +
        risk_timings.evidence_verifier_ns +
        risk_timings.expiry_guard_ns +
        risk_timings.duplicate_guard_ns +
        risk_timings.rate_limit_guard_ns
    );
    samples->risk_snapshot_revalidation_ns.push_back(
        risk_timings.market_state_guard_ns +
        risk_timings.snapshot_freshness_guard_ns
    );
    samples->risk_vwap_revalidation_ns.push_back(
        risk_timings.vwap_revalidator_ns
    );
    samples->risk_edge_guard_group_ns.push_back(
        risk_timings.edge_guard_ns + risk_timings.max_loss_guard_ns
    );
    samples->risk_exposure_inventory_ns.push_back(
        risk_timings.exposure_guard_ns + risk_timings.inventory_guard_ns
    );
    samples->risk_partial_fill_ns.push_back(risk_timings.partial_fill_guard_ns);
    samples->risk_reservation_ns.push_back(risk_timings.reservation_book_ns);
    samples->risk_audit_publish_ns.push_back(
        risk_timings.audit_trace_ns +
        risk_timings.risk_decision_build_ns +
        risk_timings.publisher_ns +
        risk_timings.metrics_ns
    );
}

[[nodiscard]] state::MarketStateSnapshot snapshot_from_depth_view(
    const state::MarketDepthView& view,
    const signal::OpportunityIntent& intent,
    std::uint16_t leg_index
) {
    state::MarketStateSnapshot snapshot;
    if (leg_index < intent.leg_count) {
        snapshot.entity_id = intent.legs[leg_index].asset_id;
        snapshot.market_id = intent.legs[leg_index].market_id;
    }
    snapshot.version = view.book_version;
    snapshot.last_book_update_ns = view.last_ws_recv_ns;
    snapshot.live = true;
    snapshot.recovering = view.recovering;
    snapshot.closed = view.closed;
    snapshot.resolved = view.resolved;
    snapshot.crossed = view.crossed;
    snapshot.has_bid = view.bid_count > 0;
    snapshot.has_ask = view.ask_count > 0;
    snapshot.bid_count = view.bid_count;
    snapshot.ask_count = view.ask_count;
    for (std::uint16_t i = 0; i < view.bid_count; ++i) {
        snapshot.bids[i] = view.bids[i];
    }
    for (std::uint16_t i = 0; i < view.ask_count; ++i) {
        snapshot.asks[i] = view.asks[i];
    }
    snapshot.best_bid_tick = view.bid_count > 0 ? view.bids[0].price_tick : 0;
    snapshot.best_ask_tick = view.ask_count > 0 ? view.asks[0].price_tick : 0;
    snapshot.snapshot_version_hash = view.snapshot_version_hash;
    snapshot.state_hash = view.snapshot_version_hash;
    snapshot.usable_for_depth = view.usable_for_depth;
    snapshot.usable_for_signal = view.usable_for_depth;
    snapshot.quality = view.usable_for_depth ? state::BookQuality::Good
                                             : state::BookQuality::Unknown;
    return snapshot;
}

void fill_execution_snapshots_from_evidence(
    const signal::SignalEvidenceView& evidence,
    const signal::OpportunityIntent& intent,
    std::vector<state::MarketStateSnapshot>* out
) {
    out->clear();
    if (evidence.snapshots != nullptr && evidence.snapshot_count > 0) {
        out->assign(
            evidence.snapshots,
            evidence.snapshots + evidence.snapshot_count
        );
        return;
    }

    if (evidence.depth_views == nullptr || evidence.depth_view_count == 0) {
        return;
    }

    out->reserve(evidence.depth_view_count);
    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto asset_index = intent.legs[i].asset_index;
        const state::MarketDepthView* matched = nullptr;
        for (std::uint16_t j = 0; j < evidence.depth_view_count; ++j) {
            if (evidence.depth_views[j].asset_index == asset_index) {
                matched = &evidence.depth_views[j];
                break;
            }
        }
        if (matched == nullptr && i < evidence.depth_view_count) {
            matched = &evidence.depth_views[i];
        }
        if (matched != nullptr) {
            out->push_back(snapshot_from_depth_view(*matched, intent, i));
        }
    }
}

[[nodiscard]] const oracle::CandidateBundle* find_bundle(
    const signal::OracleArtifactReader& artifact_reader,
    std::uint64_t bundle_id
) {
    for (const auto& bundle : artifact_reader.active_bundles()) {
        if (bundle.bundle_id == bundle_id) {
            return &bundle;
        }
    }
    return nullptr;
}

struct FastPathAssetIndex {
    std::unordered_map<std::string, std::uint32_t> asset_to_index;
    std::unordered_map<std::uint32_t, const std::string*> index_to_asset;
    std::unordered_map<std::uint32_t, const std::string*> index_to_market;
};

[[nodiscard]] FastPathAssetIndex build_fast_asset_index(
    const signal::OracleArtifactReader& artifact_reader
) {
    FastPathAssetIndex index;
    for (const auto& plan : artifact_reader.active_runtime_plans()) {
        for (std::uint16_t i = 0; i < plan.leg_count; ++i) {
            const auto* market_id = plan.market_ids[i];
            if (market_id == nullptr) {
                continue;
            }
            bool exists = false;
            for (const auto& [_, existing] : index.index_to_market) {
                if (existing != nullptr && *existing == *market_id) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                const auto market_index =
                    static_cast<std::uint32_t>(index.index_to_market.size());
                index.index_to_market.emplace(market_index, market_id);
            }
        }
        for (std::uint16_t i = 0; i < plan.unique_asset_count; ++i) {
            const auto* asset_id = plan.unique_asset_ids[i];
            if (asset_id == nullptr) {
                continue;
            }
            const auto asset_index = plan.unique_asset_indices[i];
            index.asset_to_index.emplace(*asset_id, asset_index);
            index.index_to_asset.emplace(asset_index, asset_id);
        }
    }
    return index;
}

[[nodiscard]] std::uint16_t read_depth_views_for_specs(
    const fast::FixedShapeKernelRegistry& registry,
    const FastPathAssetIndex& asset_index,
    const state::MarketStateView& market_view,
    std::uint32_t dirty_asset_index,
    std::array<state::MarketDepthView, state::kMaxDepthBatchViews>* views
) {
    const auto affected = registry.specs_for_asset(dirty_asset_index);
    if (affected.empty() || views == nullptr) {
        return 0;
    }

    std::array<const std::string*, state::kMaxDepthBatchViews> asset_ids{};
    std::array<std::uint32_t, state::kMaxDepthBatchViews> indices{};
    std::uint16_t count = 0;

    for (const auto& spec : affected) {
        for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
            const auto current_index = spec.asset_indices[i];
            bool exists = false;
            for (std::uint16_t j = 0; j < count; ++j) {
                if (indices[j] == current_index) {
                    exists = true;
                    break;
                }
            }
            if (exists || count >= state::kMaxDepthBatchViews) {
                continue;
            }
            const auto it = asset_index.index_to_asset.find(current_index);
            if (it == asset_index.index_to_asset.end() ||
                it->second == nullptr) {
                continue;
            }
            indices[count] = current_index;
            asset_ids[count] = it->second;
            ++count;
        }
    }

    if (count == 0) {
        return 0;
    }

    return market_view.get_depth_views(
        std::span<const std::string* const>{asset_ids.data(), count},
        std::span<const std::uint32_t>{indices.data(), count},
        views->data(),
        state::kMaxDepthBatchViews
    );
}

[[nodiscard]] signal::OpportunityIntent make_fast_order_decision_intent(
    const fast::FixedShapeKernelSpec& spec,
    const oracle::CandidateBundle& bundle,
    std::span<const state::MarketDepthView> depth_views,
    std::uint64_t now_ns,
    std::uint64_t ttl_ns
) {
    signal::OpportunityIntent intent;
    intent.bundle_id = bundle.bundle_id;
    intent.status = signal::IntentStatus::PaperOpportunity;
    intent.valid_under_settlement = true;
    intent.passed_quality_gate = true;
    intent.enough_depth = true;
    intent.oracle_artifact_hash = spec.artifact_hash;
    intent.constraint_hash = spec.constraint_hash;
    intent.bundle_hash = spec.bundle_hash;
    intent.snapshot_version_hash = state::hash_depth_views(depth_views);
    intent.created_ts_ns = now_ns;
    intent.expires_at_ns = now_ns + ttl_ns;
    intent.guaranteed_payout_tick = bundle.guaranteed_payout_tick;
    intent.leg_count = bundle.leg_count;

    std::uint64_t snapshot_version = 0;
    for (std::uint16_t i = 0; i < bundle.leg_count &&
                              i < signal::kMaxIntentLegs &&
                              i < spec.leg_count;
         ++i) {
        auto& leg = intent.legs[i];
        leg.market_id = bundle.legs[i].market_id;
        leg.asset_id = bundle.legs[i].asset_id;
        leg.asset_index = spec.asset_indices[i];
        leg.side = bundle.legs[i].side;
        leg.quantity_lots = bundle.legs[i].quantity_lots > 0
            ? bundle.legs[i].quantity_lots
            : 1;
        leg.enough_depth = true;
        for (const auto& view : depth_views) {
            if (view.asset_index == leg.asset_index) {
                snapshot_version = std::max(snapshot_version, view.book_version);
                break;
            }
        }
    }
    intent.snapshot_version = snapshot_version;

    auto hash = kFnvOffset;
    mix_u64(&hash, intent.bundle_id);
    mix_u64(&hash, intent.bundle_hash);
    mix_u64(&hash, intent.snapshot_version_hash);
    mix_u64(&hash, intent.oracle_artifact_hash);
    intent.intent_id = hash == 0 ? 1 : hash;
    intent.idempotency_hash = intent.intent_id;

    auto proof_hash = kFnvOffset;
    mix_u64(&proof_hash, intent.oracle_artifact_hash);
    mix_u64(&proof_hash, intent.constraint_hash);
    mix_u64(&proof_hash, intent.bundle_hash);
    mix_u64(&proof_hash, intent.snapshot_version_hash);
    intent.proof_hash = proof_hash == 0 ? 1 : proof_hash;
    return intent;
}

void increment_order_decision_reject(
    order_decision::OrderDecisionType reason,
    Counters* counters
) {
    ++counters->order_decision_rejected;
    switch (reason) {
        case order_decision::OrderDecisionType::RejectNoDepth:
            ++counters->order_decision_rejected_no_depth;
            break;
        case order_decision::OrderDecisionType::RejectLowEdge:
            ++counters->order_decision_rejected_low_edge;
            break;
        case order_decision::OrderDecisionType::RejectInvalidBundle:
            ++counters->order_decision_rejected_invalid_bundle;
            break;
        case order_decision::OrderDecisionType::RejectUnsupportedSide:
            ++counters->order_decision_rejected_unsupported_side;
            break;
        case order_decision::OrderDecisionType::RejectRiskBudget:
            ++counters->order_decision_rejected_risk_budget;
            break;
        case order_decision::OrderDecisionType::RejectPartialFillRisk:
            ++counters->order_decision_rejected_partial_fill;
            break;
        case order_decision::OrderDecisionType::RejectExpiredIntent:
            ++counters->order_decision_rejected_expired;
            break;
        case order_decision::OrderDecisionType::RejectPriceProtection:
            ++counters->order_decision_rejected_price_protection;
            break;
        default:
            ++counters->order_decision_rejected_other;
            break;
    }
}

void increment_risk_reject(
    risk::RiskRejectReason reason,
    Counters* counters
) {
    ++counters->risk_rejected;
    switch (reason) {
        case risk::RiskRejectReason::LowTotalEdge:
        case risk::RiskRejectReason::LowUnitEdge:
        case risk::RiskRejectReason::LowEdgeBps:
            ++counters->risk_rejected_low_edge;
            break;
        case risk::RiskRejectReason::CostLimit:
        case risk::RiskRejectReason::SingleMarketExposureLimit:
        case risk::RiskRejectReason::TotalExposureLimit:
        case risk::RiskRejectReason::InventoryLimit:
            ++counters->risk_rejected_cost_limit;
            break;
        case risk::RiskRejectReason::BadMarketState:
        case risk::RiskRejectReason::MissingEvidence:
            ++counters->risk_rejected_bad_state;
            break;
        case risk::RiskRejectReason::StaleBook:
        case risk::RiskRejectReason::SnapshotSkew:
            ++counters->risk_rejected_stale;
            break;
        case risk::RiskRejectReason::CostDrift:
            ++counters->risk_rejected_cost_drift;
            break;
        case risk::RiskRejectReason::InvalidIntent:
        case risk::RiskRejectReason::ExpiredIntent:
        case risk::RiskRejectReason::MissingReservation:
            ++counters->risk_rejected_invalid_intent;
            break;
        case risk::RiskRejectReason::DuplicateIntent:
        case risk::RiskRejectReason::DuplicateReservation:
            ++counters->risk_rejected_duplicate;
            break;
        case risk::RiskRejectReason::PartialFillRisk:
            ++counters->risk_rejected_partial_fill;
            break;
        default:
            ++counters->risk_rejected_other;
            break;
    }
}

void run_probe(const Config& config, ProbeState* probe) {
    signal::OracleArtifactReader artifact_reader;
    const auto artifact = artifact_reader.load(config.oracle_artifact);
    if (!artifact.ok) {
        fail("failed to load oracle artifact: " + artifact.error);
    }

    const auto packets = load_packets(config.raw_path);
    if (packets.empty()) {
        fail("raw log contains no packets");
    }

    const auto universe = state_universe_from_artifact(artifact_reader);
    state::MarketStateEventFilter filter(universe);
    fast::FixedShapeKernelRegistry fast_registry;
    fast_registry.build_from_oracle_artifact(artifact_reader);
    const auto fast_asset_index = build_fast_asset_index(artifact_reader);

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
    signal::HashOnlySignalPublisher hash_signal_publisher(
        artifact_reader.active_bundles().size()
    );
    signal::CapturingIntentPublisher json_signal_publisher;
    NullSignalPublisher null_signal_publisher;
    signal::IIntentPublisher* signal_publisher = &hash_signal_publisher;
    if (config.publisher_mode == PublisherMode::Jsonl) {
        signal_publisher = &json_signal_publisher;
    } else if (config.publisher_mode == PublisherMode::Null) {
        signal_publisher = &null_signal_publisher;
    }

    signal::SignalConfig signal_config;
    signal_config.emit_rejections = true;
    signal_config.max_lob_age_ns = 30 * static_cast<std::int64_t>(kNsPerSecond);
    signal_config.max_snapshot_version_skew = 1'000'000;
    signal_config.max_intents_per_second = 1'000'000;
    signal_config.max_intents_per_scan = 1024;
    signal_config.intent_ttl_ns = config.intent_ttl_ms * kNsPerMs;

    signal::SignalEngine signal_engine(
        signal_config,
        &snapshot_reader,
        &artifact_reader,
        &settlement_checker,
        &vwap,
        &edge_calculator,
        &ranker,
        signal_publisher
    );

    risk::HashOnlyRiskPublisher hash_risk_publisher(packets.size());
    risk::CapturingRiskPublisherFast json_risk_publisher;
    json_risk_publisher.reserve(packets.size());
    NullRiskPublisher null_risk_publisher;
    risk::IRiskDecisionPublisher* risk_publisher = &hash_risk_publisher;
    if (config.publisher_mode == PublisherMode::Jsonl) {
        risk_publisher = &json_risk_publisher;
    } else if (config.publisher_mode == PublisherMode::Null) {
        risk_publisher = &null_risk_publisher;
    }
    auto risk_policy = risk::with_computed_policy_hash(
        risk::RiskPolicySnapshot{}
    );
    risk_policy.max_book_age_ns = 30 * static_cast<std::int64_t>(kNsPerSecond);
    risk_policy.max_intent_age_ns = 30 * static_cast<std::int64_t>(kNsPerSecond);
    risk_policy.max_snapshot_skew_ns = 1'000'000;
    risk_policy.max_allowed_cost_drift_tick = 1'000'000'000;
    risk_policy.min_depth_margin_ratio = 1.0;
    risk_policy.min_depth_margin_bps = 10'000;
    risk_policy.max_approvals_per_second = 1'000'000;
    risk::RiskRuntimeContext risk_runtime;
    risk_runtime.policy = &risk_policy;
    risk_runtime.publisher = risk_publisher;
    risk::RiskEngine risk_engine(risk_runtime);

    order_decision::OrderDecisionConfig order_decision_config;
    order_decision_config.default_ttl_ns =
        config.intent_ttl_ms * kNsPerMs;
    order_decision::OrderDecisionEngine order_decision_engine(
        order_decision_config
    );
    auto fast_order_decision_config = order_decision_config;
    fast_order_decision_config.impl_mode =
        order_decision::OrderDecisionImplMode::FastFixedShape;
    fast_order_decision_config.fast_fixed_shape_fallback_to_generic = false;
    order_decision::OrderDecisionEngine fast_order_decision_engine(
        fast_order_decision_config
    );

    execution::ExecutionPlanner planner;
    execution::PaperExecutionAdapter paper_adapter;
    execution::ExecutionConfig execution_config;
    execution_config.execution_enabled = true;
    execution_config.mode = execution::ExecutionMode::Paper;
    execution_config.paper_mode = execution::PaperExecutionMode::PaperAtomic;
    execution_config.max_child_orders_per_plan = 16;
    execution_config.max_order_age_ns =
        static_cast<std::int64_t>(config.intent_ttl_ms * kNsPerMs);
    execution_config.hot_path_trust_order_decision_hash = true;
    execution_config.hot_path_trust_approval_hash = true;
    execution_config.hot_path_skip_plan_validation = true;
    execution_config.hot_path_skip_order_strings = true;
    execution_config.hot_path_numeric_plan_id = true;

    std::uint64_t scan_id = 1;
    std::uint64_t pass_events_seen = 0;

    for (std::uint64_t repeat = 0; repeat < config.repeat; ++repeat) {
        for (const auto& packet : packets) {
            ++probe->counters.raw_packets;
            decode::NormalizedEventBatch batch;
            const auto decoded = decode_pipeline.decode(
                feed::to_decode_input_view(packet),
                &batch
            );
            probe->counters.normalized_events +=
                static_cast<std::uint64_t>(batch.size());
            if (!decoded.ok() &&
                decoded.payload_kind != decode::JsonDecodeKind::NonJsonControl) {
                ++probe->counters.decode_errors;
            }

            const auto state_events = state::from_normalized_batch(batch);
            for (const auto& event : state_events) {
                ++probe->counters.filter_events_seen;
                const auto filter_result = filter.filter(event);
                if (!filter_result.passed()) {
                    ++probe->counters.filter_events_filtered;
                    continue;
                }

                ++pass_events_seen;
                ++probe->counters.filter_events_passed;
                count_pass_event_type(event, &probe->counters);
                const bool record = pass_events_seen >
                    config.warmup_pass_events;

                const auto filter_pass_ns = now_ns();
                const auto filter_pass_cycles = read_tsc();

                const auto apply_start_ns = now_ns();
                const auto filter_to_state_dispatch_ns =
                    checked_sub(apply_start_ns, filter_pass_ns);
                const auto apply_result = market_store.apply(event);
                const auto apply_end_ns = now_ns();
                const auto apply_ns = checked_sub(apply_end_ns, apply_start_ns);
                if (!apply_result.ok()) {
                    ++probe->counters.state_errors;
                }
                if (apply_result.snapshot_published) {
                    ++probe->counters.snapshots_published;
                    if (event.type == state::MarketStateEventType::WsHeartbeat) {
                        ++probe->counters.heartbeat_snapshots_published;
                    }
                }
                record_state_apply_counters(
                    event,
                    apply_result,
                    &probe->counters
                );
                if (record) {
                    record_state_apply_sample(
                        event,
                        apply_ns,
                        apply_result,
                        &probe->samples
                    );
                }

                if (config.decision_path == DecisionPathMode::FastKernel) {
                    const auto dirty_it =
                        fast_asset_index.asset_to_index.find(
                            filter_result.asset_id
                        );
                    if (dirty_it == fast_asset_index.asset_to_index.end()) {
                        continue;
                    }

                    const auto dirty_asset_index = dirty_it->second;
                    const auto affected =
                        fast_registry.specs_for_asset(dirty_asset_index);
                    if (affected.empty()) {
                        continue;
                    }

                    const auto fast_depth_start_ns = apply_end_ns;
                    std::array<state::MarketDepthView, state::kMaxDepthBatchViews>
                        fast_depth_views{};
                    const auto fast_depth_count = read_depth_views_for_specs(
                        fast_registry,
                        fast_asset_index,
                        market_view,
                        dirty_asset_index,
                        &fast_depth_views
                    );
                    const auto fast_kernel_start_ns = now_ns();
                    const auto fast_depth_read_ns = checked_sub(
                        fast_kernel_start_ns,
                        fast_depth_start_ns
                    );
                    if (fast_depth_count == 0) {
                        continue;
                    }

                    order_decision::OrderDecisionResult best_decision_result;
                    signal::OpportunityIntent best_intent;
                    const fast::FixedShapeKernelSpec* best_spec = nullptr;
                    const oracle::CandidateBundle* best_bundle = nullptr;
                    bool have_best_decision = false;
                    for (const auto& spec : affected) {
                        const auto* bundle = find_bundle(
                            artifact_reader,
                            spec.bundle_id
                        );
                        if (bundle == nullptr) {
                            continue;
                        }
                        auto intent = make_fast_order_decision_intent(
                            spec,
                            *bundle,
                            std::span<const state::MarketDepthView>{
                                fast_depth_views.data(),
                                fast_depth_count
                            },
                            event.recv_monotonic_ns != 0
                                ? event.recv_monotonic_ns
                                : fast_kernel_start_ns,
                            config.intent_ttl_ms * kNsPerMs
                        );
                        auto candidate = fast_order_decision_engine.decide(
                            intent,
                            *bundle,
                            std::span<const state::MarketDepthView>{
                                fast_depth_views.data(),
                                fast_depth_count
                            },
                            risk_policy,
                            event.recv_monotonic_ns != 0
                                ? event.recv_monotonic_ns
                                : fast_kernel_start_ns
                        );
                        if (!candidate.ok) {
                            continue;
                        }
                        if (!have_best_decision ||
                            candidate.decision.total_edge_tick >
                                best_decision_result.decision.total_edge_tick ||
                            (candidate.decision.total_edge_tick ==
                                 best_decision_result.decision.total_edge_tick &&
                             candidate.decision.bundle_id <
                                 best_decision_result.decision.bundle_id)) {
                            best_decision_result = std::move(candidate);
                            best_intent = std::move(intent);
                            best_spec = &spec;
                            best_bundle = bundle;
                            have_best_decision = true;
                        }
                    }
                    const auto fast_kernel_end_ns = now_ns();
                    const auto fast_kernel_sample_ns = checked_sub(
                        fast_kernel_end_ns,
                        fast_kernel_start_ns
                    );
                    if (!have_best_decision || best_spec == nullptr ||
                        best_bundle == nullptr) {
                        continue;
                    }

                    ++probe->counters.paper_opportunities;
                    ++probe->counters.order_decisions_created;

                    risk::RiskInputView risk_input;
                    risk_input.intent = &best_intent;
                    risk_input.depth_views = fast_depth_views.data();
                    risk_input.depth_view_count = fast_depth_count;
                    risk_input.snapshot_version_hash =
                        best_intent.snapshot_version_hash;
                    risk_input.now_ns = event.recv_monotonic_ns != 0
                        ? event.recv_monotonic_ns
                        : fast_kernel_start_ns;
                    risk_input.policy = &risk_policy;

                    const auto risk_result = risk_engine.evaluate_decision(
                        best_intent,
                        best_decision_result.decision,
                        risk_input
                    );
                    ++probe->counters.risk_evaluated;
                    if (risk_result.cost.vwap_mode ==
                        risk::RiskVWAPMode::ReuseSignalSnapshot) {
                        ++probe->counters.risk_vwap_reused_signal_snapshot;
                        ++probe->counters.risk_vwap_reused_signal_cost;
                        ++probe->counters.risk_snapshot_fast_path;
                    } else if (risk_result.cost.vwap_mode ==
                               risk::RiskVWAPMode::ReusedSignalCost) {
                        ++probe->counters.risk_vwap_reused_signal_cost;
                        ++probe->counters.risk_snapshot_fast_path;
                    } else {
                        ++probe->counters.risk_vwap_recomputed;
                        ++probe->counters.risk_snapshot_requery;
                    }
                    if (!risk_result.decision.approved()) {
                        if (probe->counters.first_risk_reject_detail.empty()) {
                            probe->counters.first_risk_reject_detail =
                                risk_result.decision.reject_detail;
                        }
                        increment_risk_reject(
                            risk_result.decision.reject_reason,
                            &probe->counters
                        );
                        continue;
                    }

                    ++probe->counters.risk_approved;

                    const auto plan_start_ns = now_ns();
                    const auto plan_cycles = read_tsc();
                    auto execution_intent = best_intent;
                    if (execution_intent.idempotency_key.empty() &&
                        execution_intent.idempotency_hash != 0) {
                        execution_intent.idempotency_key =
                            std::to_string(execution_intent.idempotency_hash);
                    }

                    risk::ApprovedIntent fast_approved;
                    fast_approved.intent = execution_intent;
                    fast_approved.decision = risk_result.decision;
                    if (fast_approved.decision.decision_id == 0) {
                        fast_approved.decision.decision_id =
                            risk_result.output_hash != 0
                                ? risk_result.output_hash
                                : execution_intent.intent_id;
                    }
                    fast_approved.reservation_id =
                        std::to_string(risk_result.reservation.reservation_id);
                    fast_approved.reservation_id_hash =
                        risk_result.reservation.reservation_id;
                    fast_approved.approved_at_ns = risk_input.now_ns;
                    fast_approved.expires_at_ns = execution_intent.expires_at_ns;

                    const auto fast_envelope =
                        order_decision::make_approved_order_decision_envelope(
                            std::move(fast_approved),
                            best_decision_result.decision,
                            event.recv_monotonic_ns != 0
                                ? event.recv_monotonic_ns
                                : fast_kernel_start_ns
                        );
                    const auto fast_plan_result = planner.build_plan(
                        fast_envelope,
                        event.recv_monotonic_ns != 0
                            ? event.recv_monotonic_ns
                            : fast_kernel_start_ns,
                        execution_config
                    );
                    const auto plan_end_ns = now_ns();
                    const auto plan_build_sample_ns =
                        checked_sub(plan_end_ns, plan_start_ns);
                    if (!fast_plan_result.ok) {
                        ++probe->counters.plan_build_rejected;
                        continue;
                    }
                    ++probe->counters.plans_built;

                    const auto execution_context_start_ns = plan_end_ns;
                    execution::ExecutionContext execution_context;
                    execution_context.now_ns =
                        event.recv_monotonic_ns != 0
                            ? event.recv_monotonic_ns
                            : fast_kernel_start_ns;
                    execution_context.config = execution_config;
                    execution_context.depth_views = fast_depth_views.data();
                    execution_context.depth_view_count = fast_depth_count;
                    const auto execution_submit_start_ns = now_ns();
                    const auto execution_context_build_ns = checked_sub(
                        execution_submit_start_ns,
                        execution_context_start_ns
                    );
                    const auto execution_result = paper_adapter.submit_plan(
                        fast_plan_result.plan,
                        execution_context
                    );
                    const auto execution_reports =
                        paper_adapter.poll_reports();
                    const auto execution_end_ns = now_ns();
                    const auto execution_submit_fill_sample_ns = checked_sub(
                        execution_end_ns,
                        execution_submit_start_ns
                    );
                    ++probe->counters.execution_submitted;
                    probe->counters.execution_reports +=
                        static_cast<std::uint64_t>(execution_reports.size());
                    for (const auto& report : execution_reports) {
                        if (report.reject_reason == "MissingSnapshot") {
                            ++probe->counters
                                  .execution_rejected_missing_snapshot;
                        } else if (
                            report.reject_reason == "InsufficientDepth") {
                            ++probe->counters
                                  .execution_rejected_insufficient_depth;
                        } else if (report.reject_reason == "Expired") {
                            ++probe->counters.execution_rejected_expired;
                        } else if (
                            report.reject_reason == "UnsupportedSide") {
                            ++probe->counters
                                  .execution_rejected_unsupported_side;
                        } else if (!report.reject_reason.empty()) {
                            ++probe->counters.execution_rejected_other;
                        }
                    }
                    if (execution_result.status ==
                        execution::PlanStatus::Filled) {
                        ++probe->counters.plans_filled;
                    } else {
                        ++probe->counters.execution_rejected;
                    }

                    if (record) {
                        const auto filter_to_plan_sample_ns =
                            checked_sub(plan_end_ns, filter_pass_ns);
                        const auto filter_to_order_filled_sample_ns =
                            checked_sub(execution_end_ns, filter_pass_ns);
                        const auto plan_to_order_filled_sample_ns =
                            checked_sub(execution_end_ns, plan_end_ns);
                        const auto overall_stage_sum_sample_ns =
                            apply_ns + fast_depth_read_ns +
                            fast_kernel_sample_ns +
                            risk_result.result.stage_timings.total_ns +
                            plan_build_sample_ns +
                            execution_context_build_ns +
                            execution_submit_fill_sample_ns;
                        const auto overall_unattributed_sample_ns =
                            filter_to_order_filled_sample_ns >
                                    overall_stage_sum_sample_ns
                                ? filter_to_order_filled_sample_ns -
                                      overall_stage_sum_sample_ns
                                : 0;

                        probe->samples.state_apply_ns.push_back(apply_ns);
                        probe->samples.fast_depth_read_ns.push_back(
                            fast_depth_read_ns
                        );
                        probe->samples.fast_kernel_ns.push_back(
                            fast_kernel_sample_ns
                        );
                        probe->samples.order_decision_ns.push_back(
                            fast_kernel_sample_ns
                        );
                        probe->samples.risk_eval_ns.push_back(
                            risk_result.result.stage_timings.total_ns
                        );
                        record_order_decision_timings(
                            best_decision_result.timings,
                            &probe->samples
                        );
                        record_order_decision_eval_stats(
                            best_decision_result.eval_stats,
                            &probe->samples
                        );
                        record_risk_timings(
                            risk_result.result.stage_timings,
                            &probe->samples
                        );
                        probe->samples.plan_build_ns.push_back(
                            plan_build_sample_ns
                        );
                        probe->samples.execution_context_build_ns.push_back(
                            execution_context_build_ns
                        );
                        probe->samples.execution_submit_fill_ns.push_back(
                            execution_submit_fill_sample_ns
                        );
                        if (execution_result.status ==
                            execution::PlanStatus::Filled) {
                            probe->samples.plan_to_order_filled_ns.push_back(
                                plan_to_order_filled_sample_ns
                            );
                            probe->samples.filter_to_order_filled_ns.push_back(
                                filter_to_order_filled_sample_ns
                            );
                        }
                        probe->samples.overall_total_ns.push_back(
                            filter_to_order_filled_sample_ns
                        );
                        probe->samples.overall_stage_sum_ns.push_back(
                            overall_stage_sum_sample_ns
                        );
                        probe->samples.overall_unattributed_ns.push_back(
                            overall_unattributed_sample_ns
                        );
                        probe->samples.filter_to_state_dispatch_ns.push_back(
                            filter_to_state_dispatch_ns
                        );
                        probe->samples.state_to_signal_dispatch_ns.push_back(0);
                        probe->samples
                            .signal_to_risk_handoff_build_ns
                            .push_back(0);
                        probe->samples.risk_input_view_build_ns.push_back(0);
                        probe->samples.risk_context_requery_ns.push_back(0);
                        probe->samples.risk_context_copy_ns.push_back(0);
                        probe->samples
                            .risk_to_execution_envelope_build_ns
                            .push_back(0);
                        probe->samples
                            .overall_unattributed_residual_ns
                            .push_back(overall_unattributed_sample_ns);
                        probe->samples.filter_to_plan_ns.push_back(
                            filter_to_plan_sample_ns
                        );
                        if (tsc_supported() &&
                            plan_cycles >= filter_pass_cycles) {
                            probe->samples.filter_to_plan_cycles.push_back(
                                plan_cycles - filter_pass_cycles
                            );
                        }
                    }

                    if (config.max_plan_samples > 0 &&
                        probe->samples.filter_to_plan_ns.size() >=
                            config.max_plan_samples) {
                        return;
                    }
                    continue;
                }

                const auto state_to_signal_dispatch_start_ns = apply_end_ns;
                signal::SignalScanContext scan_context;
                scan_context.scan_id = scan_id++;
                scan_context.now_monotonic_ns =
                    event.recv_monotonic_ns != 0
                        ? event.recv_monotonic_ns
                        : state_to_signal_dispatch_start_ns;
                scan_context.settlement_masks_available = false;
                const auto scan_start_ns = now_ns();
                const auto state_to_signal_dispatch_ns =
                    checked_sub(
                        scan_start_ns,
                        state_to_signal_dispatch_start_ns
                    );
                const auto signal_result = signal_engine.scan_once(scan_context);
                const auto scan_end_ns = now_ns();
                ++probe->counters.signal_scans;
                probe->counters.paper_opportunities +=
                    signal_result.paper_opportunities;
                probe->counters.signal_rejections +=
                    signal_result.intents_published >=
                            signal_result.paper_opportunities
                        ? signal_result.intents_published -
                              signal_result.paper_opportunities
                        : 0;

                if (record) {
                    probe->samples.pass_event_to_scan_ns.push_back(
                        checked_sub(scan_end_ns, filter_pass_ns)
                    );
                }

                const auto intents = signal_engine.last_published_intents();
                for (std::size_t intent_index = 0; intent_index < intents.size();
                     ++intent_index) {
                    const auto signal_to_risk_handoff_start_ns = now_ns();
                    const auto& intent = intents[intent_index];
                    const auto evidence = signal_engine.last_published_evidence_at(
                        intent_index
                    );
                    if (intent.status != signal::IntentStatus::PaperOpportunity) {
                        continue;
                    }

                    const auto handoff = signal::make_signal_risk_handoff(
                        intent,
                        evidence,
                        scan_context.now_monotonic_ns
                    );
                    const auto risk_input_view_start_ns = now_ns();
                    const auto signal_to_risk_handoff_build_ns =
                        checked_sub(
                            risk_input_view_start_ns,
                            signal_to_risk_handoff_start_ns
                        );
                    auto risk_input =
                        risk::make_risk_input_view(handoff);
                    risk_input.policy = &risk_policy;
                    const auto risk_context_requery_ns = 0ULL;
                    const auto risk_context_copy_ns = 0ULL;

                    const auto risk_input_view_build_ns =
                        checked_sub(now_ns(), risk_input_view_start_ns);
                    const auto order_decision_start_ns = now_ns();
                    const auto* bundle = find_bundle(
                        artifact_reader,
                        intent.bundle_id
                    );
                    if (bundle == nullptr || evidence.depth_views == nullptr) {
                        increment_order_decision_reject(
                            order_decision::OrderDecisionType::
                                RejectInvalidBundle,
                            &probe->counters
                        );
                        continue;
                    }
                    const auto decision_result = order_decision_engine.decide(
                        intent,
                        *bundle,
                        std::span<const state::MarketDepthView>{
                            evidence.depth_views,
                            evidence.depth_view_count
                        },
                        risk_policy,
                        scan_context.now_monotonic_ns
                    );
                    const auto order_decision_end_ns = now_ns();
                    const auto order_decision_sample_ns = checked_sub(
                        order_decision_end_ns,
                        order_decision_start_ns
                    );
                    if (!decision_result.ok) {
                        increment_order_decision_reject(
                            decision_result.reject_reason,
                            &probe->counters
                        );
                        if (record) {
                            probe->samples.order_decision_ns.push_back(
                                order_decision_sample_ns
                            );
                            record_order_decision_timings(
                                decision_result.timings,
                                &probe->samples
                            );
                            record_order_decision_eval_stats(
                                decision_result.eval_stats,
                                &probe->samples
                            );
                        }
                        continue;
                    }
                    ++probe->counters.order_decisions_created;

                    const auto risk_start_ns = order_decision_end_ns;
                    const auto risk_result = risk_engine.evaluate_decision(
                        intent,
                        decision_result.decision,
                        risk_input
                    );
                    const auto risk_end_ns = now_ns();
                    const auto risk_to_execution_start_ns = risk_end_ns;
                    ++probe->counters.risk_evaluated;
                    if (risk_result.cost.vwap_mode ==
                        risk::RiskVWAPMode::ReuseSignalSnapshot) {
                        ++probe->counters.risk_vwap_reused_signal_snapshot;
                        ++probe->counters.risk_vwap_reused_signal_cost;
                        ++probe->counters.risk_snapshot_fast_path;
                    } else if (risk_result.cost.vwap_mode ==
                               risk::RiskVWAPMode::ReusedSignalCost) {
                        ++probe->counters.risk_vwap_reused_signal_cost;
                        ++probe->counters.risk_snapshot_fast_path;
                    } else {
                        ++probe->counters.risk_vwap_recomputed;
                        ++probe->counters.risk_snapshot_requery;
                    }
                    if (!risk_result.decision.approved()) {
                        if (probe->counters.first_risk_reject_detail.empty()) {
                            probe->counters.first_risk_reject_detail =
                                risk_result.decision.reject_detail;
                        }
                        increment_risk_reject(
                            risk_result.decision.reject_reason,
                            &probe->counters
                        );
                        continue;
                    }
                    ++probe->counters.risk_approved;

                    auto execution_intent = intent;
                    if (execution_intent.idempotency_key.empty() &&
                        execution_intent.idempotency_hash != 0) {
                        execution_intent.idempotency_key =
                            std::to_string(execution_intent.idempotency_hash);
                    }

                    risk::ApprovedIntent approved_intent;
                    approved_intent.intent = execution_intent;
                    approved_intent.decision = risk_result.decision;
                    if (approved_intent.decision.decision_id == 0) {
                        approved_intent.decision.decision_id =
                            risk_result.output_hash != 0
                                ? risk_result.output_hash
                                : intent.intent_id;
                    }
                    approved_intent.reservation_id =
                        std::to_string(risk_result.reservation.reservation_id);
                    approved_intent.reservation_id_hash =
                        risk_result.reservation.reservation_id;
                    approved_intent.approved_at_ns =
                        scan_context.now_monotonic_ns;
                    approved_intent.expires_at_ns = execution_intent.expires_at_ns;

                    const auto envelope =
                        order_decision::make_approved_order_decision_envelope(
                            std::move(approved_intent),
                            decision_result.decision,
                            scan_context.now_monotonic_ns
                        );

                    const auto plan_start_ns = now_ns();
                    const auto risk_to_execution_envelope_build_ns =
                        checked_sub(plan_start_ns, risk_to_execution_start_ns);
                    const auto plan_result = planner.build_plan(
                        envelope,
                        scan_context.now_monotonic_ns,
                        execution_config
                    );
                    const auto plan_cycles = read_tsc();
                    const auto plan_end_ns = now_ns();

                    if (!plan_result.ok) {
                        ++probe->counters.plan_build_rejected;
                        continue;
                    }

                    ++probe->counters.plans_built;

                    const auto execution_context_start_ns = plan_end_ns;
                    execution::ExecutionContext execution_context;
                    execution_context.now_ns = scan_context.now_monotonic_ns;
                    execution_context.config = execution_config;
                    execution_context.depth_views = evidence.depth_views;
                    execution_context.depth_view_count =
                        evidence.depth_view_count;
                    if (execution_context.depth_views == nullptr ||
                        execution_context.depth_view_count == 0) {
                        fill_execution_snapshots_from_evidence(
                            evidence,
                            intent,
                            &execution_context.snapshots
                        );
                    }
                    const auto execution_submit_start_ns = now_ns();
                    const auto execution_context_build_ns = checked_sub(
                        execution_submit_start_ns,
                        execution_context_start_ns
                    );
                    const auto execution_result = paper_adapter.submit_plan(
                        plan_result.plan,
                        execution_context
                    );
                    const auto execution_reports =
                        paper_adapter.poll_reports();
                    const auto execution_end_ns = now_ns();
                    const auto execution_submit_fill_sample_ns = checked_sub(
                        execution_end_ns,
                        execution_submit_start_ns
                    );
                    ++probe->counters.execution_submitted;
                    probe->counters.execution_reports +=
                        static_cast<std::uint64_t>(execution_reports.size());
                    for (const auto& report : execution_reports) {
                        if (report.reject_reason == "MissingSnapshot") {
                            ++probe->counters
                                  .execution_rejected_missing_snapshot;
                        } else if (
                            report.reject_reason == "InsufficientDepth") {
                            ++probe->counters
                                  .execution_rejected_insufficient_depth;
                        } else if (report.reject_reason == "Expired") {
                            ++probe->counters.execution_rejected_expired;
                        } else if (
                            report.reject_reason == "UnsupportedSide") {
                            ++probe->counters
                                  .execution_rejected_unsupported_side;
                        } else if (!report.reject_reason.empty()) {
                            ++probe->counters.execution_rejected_other;
                        }
                    }
                    if (execution_result.status ==
                        execution::PlanStatus::Filled) {
                        ++probe->counters.plans_filled;
                    } else {
                        ++probe->counters.execution_rejected;
                    }

                    if (record) {
                        const auto signal_scan_sample_ns =
                            checked_sub(scan_end_ns, scan_start_ns);
                        const auto risk_eval_sample_ns =
                            checked_sub(risk_end_ns, risk_start_ns);
                        const auto plan_build_sample_ns =
                            checked_sub(plan_end_ns, plan_start_ns);
                        const auto filter_to_plan_sample_ns =
                            checked_sub(plan_end_ns, filter_pass_ns);
                        const auto filter_to_order_filled_sample_ns =
                            checked_sub(execution_end_ns, filter_pass_ns);
                        const auto plan_to_order_filled_sample_ns =
                            checked_sub(execution_end_ns, plan_start_ns);
                        const auto overall_stage_sum_sample_ns =
                            apply_ns + signal_scan_sample_ns +
                            order_decision_sample_ns + risk_eval_sample_ns +
                            plan_build_sample_ns + execution_context_build_ns +
                            execution_submit_fill_sample_ns;
                        const auto overall_unattributed_sample_ns =
                            filter_to_order_filled_sample_ns >
                                    overall_stage_sum_sample_ns
                                ? filter_to_order_filled_sample_ns -
                                      overall_stage_sum_sample_ns
                                : 0;
                        const auto overall_dispatch_sum_sample_ns =
                            filter_to_state_dispatch_ns +
                            state_to_signal_dispatch_ns +
                            signal_to_risk_handoff_build_ns +
                            risk_input_view_build_ns +
                            risk_context_requery_ns +
                            risk_context_copy_ns +
                            risk_to_execution_envelope_build_ns;
                        const auto overall_unattributed_residual_sample_ns =
                            overall_unattributed_sample_ns >
                                    overall_dispatch_sum_sample_ns
                                ? overall_unattributed_sample_ns -
                                      overall_dispatch_sum_sample_ns
                                : 0;
                        probe->samples.state_apply_ns.push_back(
                            apply_ns
                        );
                        probe->samples.signal_scan_ns.push_back(
                            signal_scan_sample_ns
                        );
                        probe->samples.signal_bundle_scan_ns.push_back(
                            signal_result.stage_timings.bundle_scan_ns
                        );
                        probe->samples.signal_settlement_check_ns.push_back(
                            signal_result.stage_timings.settlement_check_ns
                        );
                        probe->samples.signal_snapshot_reader_ns.push_back(
                            signal_result.stage_timings.snapshot_reader_ns
                        );
                        probe->samples
                            .signal_snapshot_consistency_guard_ns
                            .push_back(
                                signal_result.stage_timings
                                    .snapshot_consistency_guard_ns
                            );
                        probe->samples
                            .signal_price_vector_builder_ns
                            .push_back(
                                signal_result.stage_timings
                                    .price_vector_builder_ns
                            );
                        probe->samples.signal_vwap_precheck_ns.push_back(
                            signal_result.stage_timings.vwap_precheck_ns
                        );
                        probe->samples.signal_edge_calculator_ns.push_back(
                            signal_result.stage_timings.edge_calculator_ns
                        );
                        probe->samples.signal_intent_builder_ns.push_back(
                            signal_result.stage_timings.intent_builder_ns
                        );
                        probe->samples.signal_dedupe_ns.push_back(
                            signal_result.stage_timings.dedupe_ns
                        );
                        probe->samples.signal_rate_limiter_ns.push_back(
                            signal_result.stage_timings.rate_limiter_ns
                        );
                        probe->samples.signal_publisher_ns.push_back(
                            signal_result.stage_timings.publisher_ns
                        );
                        probe->samples.risk_eval_ns.push_back(
                            risk_eval_sample_ns
                        );
                        probe->samples.order_decision_ns.push_back(
                            order_decision_sample_ns
                        );
                        record_order_decision_timings(
                            decision_result.timings,
                            &probe->samples
                        );
                        record_order_decision_eval_stats(
                            decision_result.eval_stats,
                            &probe->samples
                        );
                        const auto& risk_timings =
                            risk_result.result.stage_timings;
                        probe->samples.risk_total_ns.push_back(
                            risk_timings.total_ns
                        );
                        probe->samples.risk_stage_sum_ns.push_back(
                            risk_timings.stage_sum_ns
                        );
                        probe->samples.risk_unattributed_ns.push_back(
                            risk_timings.unattributed_ns
                        );
                        probe->samples.risk_kill_switch_guard_ns.push_back(
                            risk_timings.kill_switch_guard_ns
                        );
                        probe->samples.risk_intent_validator_ns.push_back(
                            risk_timings.intent_validator_ns
                        );
                        probe->samples.risk_evidence_verifier_ns.push_back(
                            risk_timings.evidence_verifier_ns
                        );
                        probe->samples.risk_expiry_guard_ns.push_back(
                            risk_timings.expiry_guard_ns
                        );
                        probe->samples.risk_duplicate_guard_ns.push_back(
                            risk_timings.duplicate_guard_ns
                        );
                        probe->samples.risk_rate_limit_guard_ns.push_back(
                            risk_timings.rate_limit_guard_ns
                        );
                        probe->samples.risk_market_state_guard_ns.push_back(
                            risk_timings.market_state_guard_ns
                        );
                        probe->samples
                            .risk_snapshot_freshness_guard_ns
                            .push_back(
                                risk_timings.snapshot_freshness_guard_ns
                            );
                        probe->samples.risk_cost_revalidator_ns.push_back(
                            risk_timings.cost_revalidator_ns
                        );
                        probe->samples.risk_vwap_revalidator_ns.push_back(
                            risk_timings.vwap_revalidator_ns
                        );
                        probe->samples.risk_edge_guard_ns.push_back(
                            risk_timings.edge_guard_ns
                        );
                        probe->samples.risk_max_loss_guard_ns.push_back(
                            risk_timings.max_loss_guard_ns
                        );
                        probe->samples.risk_exposure_guard_ns.push_back(
                            risk_timings.exposure_guard_ns
                        );
                        probe->samples.risk_inventory_guard_ns.push_back(
                            risk_timings.inventory_guard_ns
                        );
                        probe->samples.risk_partial_fill_guard_ns.push_back(
                            risk_timings.partial_fill_guard_ns
                        );
                        probe->samples.risk_reservation_book_ns.push_back(
                            risk_timings.reservation_book_ns
                        );
                        probe->samples.risk_audit_trace_ns.push_back(
                            risk_timings.audit_trace_ns
                        );
                        probe->samples.risk_decision_build_ns.push_back(
                            risk_timings.risk_decision_build_ns
                        );
                        probe->samples.risk_publisher_ns.push_back(
                            risk_timings.publisher_ns
                        );
                        probe->samples.risk_metrics_ns.push_back(
                            risk_timings.metrics_ns
                        );
                        probe->samples.risk_cheap_guards_ns.push_back(
                            risk_timings.kill_switch_guard_ns +
                            risk_timings.intent_validator_ns +
                            risk_timings.evidence_verifier_ns +
                            risk_timings.expiry_guard_ns +
                            risk_timings.duplicate_guard_ns +
                            risk_timings.rate_limit_guard_ns
                        );
                        probe->samples
                            .risk_snapshot_revalidation_ns
                            .push_back(
                                risk_timings.market_state_guard_ns +
                                risk_timings.snapshot_freshness_guard_ns
                            );
                        probe->samples.risk_vwap_revalidation_ns.push_back(
                            risk_timings.vwap_revalidator_ns
                        );
                        probe->samples.risk_edge_guard_group_ns.push_back(
                            risk_timings.edge_guard_ns +
                            risk_timings.max_loss_guard_ns
                        );
                        probe->samples.risk_exposure_inventory_ns.push_back(
                            risk_timings.exposure_guard_ns +
                            risk_timings.inventory_guard_ns
                        );
                        probe->samples.risk_partial_fill_ns.push_back(
                            risk_timings.partial_fill_guard_ns
                        );
                        probe->samples.risk_reservation_ns.push_back(
                            risk_timings.reservation_book_ns
                        );
                        probe->samples.risk_audit_publish_ns.push_back(
                            risk_timings.audit_trace_ns +
                            risk_timings.risk_decision_build_ns +
                            risk_timings.publisher_ns +
                            risk_timings.metrics_ns
                        );
                        probe->samples.plan_build_ns.push_back(
                            plan_build_sample_ns
                        );
                        probe->samples.execution_context_build_ns.push_back(
                            execution_context_build_ns
                        );
                        probe->samples.execution_submit_fill_ns.push_back(
                            execution_submit_fill_sample_ns
                        );
                        if (execution_result.status ==
                            execution::PlanStatus::Filled) {
                            probe->samples.plan_to_order_filled_ns.push_back(
                                plan_to_order_filled_sample_ns
                            );
                            probe->samples.filter_to_order_filled_ns.push_back(
                                filter_to_order_filled_sample_ns
                            );
                        }
                        probe->samples.overall_total_ns.push_back(
                            filter_to_order_filled_sample_ns
                        );
                        probe->samples.overall_stage_sum_ns.push_back(
                            overall_stage_sum_sample_ns
                        );
                        probe->samples.overall_unattributed_ns.push_back(
                            overall_unattributed_sample_ns
                        );
                        probe->samples.filter_to_state_dispatch_ns.push_back(
                            filter_to_state_dispatch_ns
                        );
                        probe->samples.state_to_signal_dispatch_ns.push_back(
                            state_to_signal_dispatch_ns
                        );
                        probe->samples
                            .signal_to_risk_handoff_build_ns
                            .push_back(signal_to_risk_handoff_build_ns);
                        probe->samples.risk_input_view_build_ns.push_back(
                            risk_input_view_build_ns
                        );
                        probe->samples.risk_context_requery_ns.push_back(
                            risk_context_requery_ns
                        );
                        probe->samples.risk_context_copy_ns.push_back(
                            risk_context_copy_ns
                        );
                        probe->samples
                            .risk_to_execution_envelope_build_ns
                            .push_back(risk_to_execution_envelope_build_ns);
                        probe->samples
                            .overall_unattributed_residual_ns
                            .push_back(
                                overall_unattributed_residual_sample_ns
                            );
                        probe->samples.filter_to_plan_ns.push_back(
                            filter_to_plan_sample_ns
                        );
                        if (tsc_supported() && plan_cycles >= filter_pass_cycles) {
                            probe->samples.filter_to_plan_cycles.push_back(
                                plan_cycles - filter_pass_cycles
                            );
                        }
                    }

                    if (config.max_plan_samples > 0 &&
                        probe->samples.filter_to_plan_ns.size() >=
                            config.max_plan_samples) {
                        return;
                    }
                }
            }
        }
    }
}

void print_state_apply_breakdown(
    const char* name,
    const StateApplyBreakdownSamples& samples
) {
    std::cout << "  " << name << ":\n";
    print_stats_with_indent("    ", "total", samples.total_ns);
    print_stats_with_indent("    ", "mutation", samples.mutation_ns);
    print_stats_with_indent("    ", "make_result", samples.make_result_ns);
    print_stats_with_indent("    ", "hash_entity", samples.hash_entity_ns);
    print_stats_with_indent("    ", "hash_global", samples.hash_global_ns);
    print_stats_with_indent(
        "    ",
        "snapshot_build",
        samples.snapshot_build_ns
    );
    print_stats_with_indent(
        "    ",
        "snapshot_publish",
        samples.snapshot_publish_ns
    );
}

void print_apply_counter_block(
    const char* name,
    const StateApplyProbeCounters& counters
) {
    std::cout << "  " << name << ":\n";
    std::cout << "    publish_skipped: "
              << counters.publish_skipped << '\n';
    std::cout << "    publish_performed: "
              << counters.publish_performed << '\n';
    std::cout << "    full_hash_computed: "
              << counters.full_hash_computed << '\n';
    std::cout << "    full_hash_skipped: "
              << counters.full_hash_skipped << '\n';
    std::cout << "    cache_hit: " << counters.cache_hit << '\n';
    std::cout << "    cache_miss: " << counters.cache_miss << '\n';
}

void print_report(const Config& config, const ProbeState& probe) {
    std::cout << "market_event_to_execution_plan_probe:\n";
    std::cout << "  raw: " << config.raw_path.string() << '\n';
    std::cout << "  oracle_artifact: "
              << config.oracle_artifact.string() << '\n';
    std::cout << "  repeat: " << config.repeat << '\n';
    std::cout << "  warmup_pass_events: "
              << config.warmup_pass_events << '\n';
    std::cout << "  publisher_mode: "
              << to_string(config.publisher_mode) << '\n';
    std::cout << "  decision_path: "
              << to_string(config.decision_path) << '\n';
    std::cout << "  check_determinism: "
              << (probe.determinism_checked ? "true" : "false") << '\n';
    std::cout << "  tsc_available: "
              << (probe.tsc_available ? "true" : "false") << '\n';
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "  tsc_cycles_per_ns: "
              << probe.tsc_cycles_per_ns << "\n\n";
    std::cout.unsetf(std::ios::floatfield);

    std::cout << "counters:\n";
    std::cout << "  raw_packets: " << probe.counters.raw_packets << '\n';
    std::cout << "  normalized_events: "
              << probe.counters.normalized_events << '\n';
    std::cout << "  decode_errors: " << probe.counters.decode_errors << '\n';
    std::cout << "  filter_events_seen: "
              << probe.counters.filter_events_seen << '\n';
    std::cout << "  filter_events_passed: "
              << probe.counters.filter_events_passed << '\n';
    std::cout << "  filter_events_filtered: "
              << probe.counters.filter_events_filtered << '\n';
    std::cout << "  state_errors: " << probe.counters.state_errors << '\n';
    std::cout << "  snapshots_published: "
              << probe.counters.snapshots_published << '\n';
    std::cout << "  heartbeat_snapshots_published: "
              << probe.counters.heartbeat_snapshots_published << '\n';
    std::cout << "  signal_scans: " << probe.counters.signal_scans << '\n';
    std::cout << "  paper_opportunities: "
              << probe.counters.paper_opportunities << '\n';
    std::cout << "  signal_rejections: "
              << probe.counters.signal_rejections << '\n';
    std::cout << "  risk_evaluated: " << probe.counters.risk_evaluated << '\n';
    std::cout << "  risk_approved: " << probe.counters.risk_approved << '\n';
    std::cout << "  risk_rejected: " << probe.counters.risk_rejected << '\n';
    std::cout << "  risk_vwap_reused_signal_cost: "
              << probe.counters.risk_vwap_reused_signal_cost << '\n';
    std::cout << "  risk_vwap_reused_signal_snapshot: "
              << probe.counters.risk_vwap_reused_signal_snapshot << '\n';
    std::cout << "  risk_vwap_recomputed: "
              << probe.counters.risk_vwap_recomputed << '\n';
    std::cout << "  risk_snapshot_fast_path: "
              << probe.counters.risk_snapshot_fast_path << '\n';
    std::cout << "  risk_snapshot_requery: "
              << probe.counters.risk_snapshot_requery << '\n';
    std::cout << "  order_decisions_created: "
              << probe.counters.order_decisions_created << '\n';
    std::cout << "  order_decision_rejected: "
              << probe.counters.order_decision_rejected << '\n';
    std::cout << "  plans_built: " << probe.counters.plans_built << '\n';
    std::cout << "  plan_build_rejected: "
              << probe.counters.plan_build_rejected << '\n';
    std::cout << "  execution_submitted: "
              << probe.counters.execution_submitted << '\n';
    std::cout << "  plans_filled: " << probe.counters.plans_filled << '\n';
    std::cout << "  execution_rejected: "
              << probe.counters.execution_rejected << '\n';
    std::cout << "  execution_reports: "
              << probe.counters.execution_reports << "\n\n";
    std::cout << "risk_rejections:\n";
    std::cout << "  low_edge: "
              << probe.counters.risk_rejected_low_edge << '\n';
    std::cout << "  cost_or_exposure_limit: "
              << probe.counters.risk_rejected_cost_limit << '\n';
    std::cout << "  bad_state_or_missing_evidence: "
              << probe.counters.risk_rejected_bad_state << '\n';
    std::cout << "  stale_or_skew: "
              << probe.counters.risk_rejected_stale << '\n';
    std::cout << "  cost_drift: "
              << probe.counters.risk_rejected_cost_drift << '\n';
    std::cout << "  invalid_intent: "
              << probe.counters.risk_rejected_invalid_intent << '\n';
    std::cout << "  duplicate: "
              << probe.counters.risk_rejected_duplicate << '\n';
    std::cout << "  partial_fill: "
              << probe.counters.risk_rejected_partial_fill << '\n';
    std::cout << "  other: "
              << probe.counters.risk_rejected_other << "\n\n";
    if (!probe.counters.first_risk_reject_detail.empty()) {
        std::cout << "  first_detail: "
                  << probe.counters.first_risk_reject_detail << "\n\n";
    }
    std::cout << "order_decision_rejections:\n";
    std::cout << "  no_depth: "
              << probe.counters.order_decision_rejected_no_depth << '\n';
    std::cout << "  low_edge: "
              << probe.counters.order_decision_rejected_low_edge << '\n';
    std::cout << "  invalid_bundle: "
              << probe.counters.order_decision_rejected_invalid_bundle << '\n';
    std::cout << "  unsupported_side: "
              << probe.counters.order_decision_rejected_unsupported_side
              << '\n';
    std::cout << "  risk_budget: "
              << probe.counters.order_decision_rejected_risk_budget << '\n';
    std::cout << "  partial_fill: "
              << probe.counters.order_decision_rejected_partial_fill << '\n';
    std::cout << "  expired: "
              << probe.counters.order_decision_rejected_expired << '\n';
    std::cout << "  price_protection: "
              << probe.counters.order_decision_rejected_price_protection
              << '\n';
    std::cout << "  other: "
              << probe.counters.order_decision_rejected_other << "\n\n";
    std::cout << "execution_rejections:\n";
    std::cout << "  missing_snapshot: "
              << probe.counters.execution_rejected_missing_snapshot << '\n';
    std::cout << "  insufficient_depth: "
              << probe.counters.execution_rejected_insufficient_depth << '\n';
    std::cout << "  expired: "
              << probe.counters.execution_rejected_expired << '\n';
    std::cout << "  unsupported_side: "
              << probe.counters.execution_rejected_unsupported_side << '\n';
    std::cout << "  other: "
              << probe.counters.execution_rejected_other << "\n\n";

    if (probe.determinism_checked) {
        std::cout << "determinism:\n";
        std::cout << "  determinism_passed: "
                  << (probe.determinism_passed ? "true" : "false")
                  << "\n\n";
    }

    std::cout << "acceptance_matrix:\n";
    std::cout << "  fixture: worldcup_feed_to_execute_30m_20260530_170629\n";
    std::cout << "  raw: " << config.raw_path.string() << '\n';
    std::cout << "  oracle_artifact: "
              << config.oracle_artifact.string() << '\n';
    std::cout << "  latency:\n";
    print_acceptance_latency(
        "filter_pass_to_plan_total",
        probe.samples.filter_to_plan_ns
    );
    print_acceptance_latency("state_apply", probe.samples.state_apply_ns);
    print_acceptance_latency("signal_scan", probe.samples.signal_scan_ns);
    print_acceptance_latency(
        "fast_depth_read",
        probe.samples.fast_depth_read_ns
    );
    print_acceptance_latency("fast_kernel", probe.samples.fast_kernel_ns);
    print_acceptance_latency(
        "order_decision",
        probe.samples.order_decision_ns
    );
    print_acceptance_latency("risk_eval", probe.samples.risk_eval_ns);
    print_acceptance_latency(
        "execution_plan_build",
        probe.samples.plan_build_ns
    );
    print_acceptance_latency(
        "filter_pass_to_order_filled_total",
        probe.samples.filter_to_order_filled_ns
    );
    print_acceptance_latency(
        "execution_submit_fill",
        probe.samples.execution_submit_fill_ns
    );
    print_acceptance_latency(
        "overall_unattributed",
        probe.samples.overall_unattributed_ns
    );
    std::cout << "  correctness:\n";
    std::cout << "    decode_errors: "
              << probe.counters.decode_errors << '\n';
    std::cout << "    state_errors: "
              << probe.counters.state_errors << '\n';
    std::cout << "    paper_opportunities: "
              << probe.counters.paper_opportunities << '\n';
    std::cout << "    risk_approved: "
              << probe.counters.risk_approved << '\n';
    std::cout << "    order_decisions_created: "
              << probe.counters.order_decisions_created << '\n';
    std::cout << "    plans_built: "
              << probe.counters.plans_built << '\n';
    std::cout << "    plans_filled: "
              << probe.counters.plans_filled << "\n\n";

    std::cout << "passed_event_types:\n";
    std::cout << "  snapshot: " << probe.counters.snapshot_pass_events << '\n';
    std::cout << "  delta: " << probe.counters.delta_pass_events << '\n';
    std::cout << "  heartbeat: " << probe.counters.heartbeat_pass_events << '\n';
    std::cout << "  other: " << probe.counters.other_pass_events << "\n\n";

    std::cout << "risk_vwap_mode:\n";
    std::cout << "  ReuseSignalSnapshot: "
              << probe.counters.risk_vwap_reused_signal_snapshot << '\n';
    std::cout << "  ReusedSignalCost: "
              << probe.counters.risk_vwap_reused_signal_cost << '\n';
    std::cout << "  RecomputedFromSnapshot: "
              << probe.counters.risk_vwap_recomputed << "\n\n";

    std::cout << "risk_snapshot_path:\n";
    std::cout << "  fast_path: "
              << probe.counters.risk_snapshot_fast_path << '\n';
    std::cout << "  requery: "
              << probe.counters.risk_snapshot_requery << "\n\n";

    std::cout << "publish:\n";
    std::cout << "  publish_skipped: "
              << probe.counters.state_apply.all.publish_skipped << '\n';
    std::cout << "  publish_performed: "
              << probe.counters.state_apply.all.publish_performed << "\n\n";

    std::cout << "hash:\n";
    std::cout << "  full_hash_computed: "
              << probe.counters.state_apply.all.full_hash_computed << '\n';
    std::cout << "  full_hash_skipped: "
              << probe.counters.state_apply.all.full_hash_skipped << '\n';
    std::cout << "  cache_hit: "
              << probe.counters.state_apply.all.cache_hit << '\n';
    std::cout << "  cache_miss: "
              << probe.counters.state_apply.all.cache_miss << "\n\n";

    std::cout << "heartbeat:\n";
    std::cout << "  publish_skipped: "
              << probe.counters.state_apply.heartbeat.publish_skipped << '\n';
    std::cout << "  publish_performed: "
              << probe.counters.state_apply.heartbeat.publish_performed << '\n';
    std::cout << "  full_hash_computed: "
              << probe.counters.state_apply.heartbeat.full_hash_computed
              << '\n';
    std::cout << "  full_hash_skipped: "
              << probe.counters.state_apply.heartbeat.full_hash_skipped
              << '\n';
    std::cout << "  cache_hit: "
              << probe.counters.state_apply.heartbeat.cache_hit << '\n';
    std::cout << "  cache_miss: "
              << probe.counters.state_apply.heartbeat.cache_miss << '\n';
    std::cout << "  hash_entity: "
              << sum_values(
                     probe.samples.state_apply_breakdown.heartbeat
                         .hash_entity_ns
                 )
              << '\n';
    std::cout << "  hash_global: "
              << sum_values(
                     probe.samples.state_apply_breakdown.heartbeat
                         .hash_global_ns
                 )
              << '\n';
    std::cout << "  hash_entity_ns_total: "
              << sum_values(
                     probe.samples.state_apply_breakdown.heartbeat
                         .hash_entity_ns
                 )
              << '\n';
    std::cout << "  hash_global_ns_total: "
              << sum_values(
                     probe.samples.state_apply_breakdown.heartbeat
                         .hash_global_ns
                 )
              << "\n\n";

    std::cout << "state_apply:\n";
    print_state_apply_breakdown(
        "all",
        probe.samples.state_apply_breakdown.all
    );
    print_state_apply_breakdown(
        "snapshot",
        probe.samples.state_apply_breakdown.snapshot
    );
    print_state_apply_breakdown(
        "delta",
        probe.samples.state_apply_breakdown.delta
    );
    print_state_apply_breakdown(
        "heartbeat",
        probe.samples.state_apply_breakdown.heartbeat
    );
    print_state_apply_breakdown(
        "other",
        probe.samples.state_apply_breakdown.other
    );
    std::cout << '\n';

    std::cout << "state_apply_counters:\n";
    print_apply_counter_block("all", probe.counters.state_apply.all);
    print_apply_counter_block("snapshot", probe.counters.state_apply.snapshot);
    print_apply_counter_block("delta", probe.counters.state_apply.delta);
    print_apply_counter_block("heartbeat", probe.counters.state_apply.heartbeat);
    print_apply_counter_block("other", probe.counters.state_apply.other);
    std::cout << '\n';

    std::cout << "latency_us:\n";
    print_stats("state_apply_all_passed", probe.samples.state_apply_all_passed_ns);
    print_stats(
        "state_mutation_ex_publish_all_passed",
        probe.samples.state_mutation_ex_publish_all_passed_ns
    );
    print_stats(
        "snapshot_publish_all_passed",
        probe.samples.snapshot_publish_all_passed_ns
    );
    print_stats(
        "snapshot_publish_actual",
        probe.samples.snapshot_publish_actual_ns
    );
    print_stats("state_apply_snapshot", probe.samples.state_apply_snapshot_ns);
    print_stats("state_apply_delta", probe.samples.state_apply_delta_ns);
    print_stats("state_apply_heartbeat", probe.samples.state_apply_heartbeat_ns);
    print_stats("state_apply_other", probe.samples.state_apply_other_ns);
    print_stats("filter_pass_to_plan_total", probe.samples.filter_to_plan_ns);
    print_stats("state_apply", probe.samples.state_apply_ns);
    print_stats("signal_scan", probe.samples.signal_scan_ns);
    print_stats("fast_depth_read", probe.samples.fast_depth_read_ns);
    print_stats("fast_kernel", probe.samples.fast_kernel_ns);
    print_stats("order_decision", probe.samples.order_decision_ns);
    print_stats("risk_eval", probe.samples.risk_eval_ns);
    print_stats("execution_plan_build", probe.samples.plan_build_ns);
    print_stats(
        "execution_context_build",
        probe.samples.execution_context_build_ns
    );
    print_stats(
        "execution_submit_fill",
        probe.samples.execution_submit_fill_ns
    );
    print_stats(
        "plan_to_order_filled",
        probe.samples.plan_to_order_filled_ns
    );
    print_stats(
        "filter_pass_to_order_filled_total",
        probe.samples.filter_to_order_filled_ns
    );
    print_stats("filter_pass_to_signal_scan", probe.samples.pass_event_to_scan_ns);

    std::cout << "\noverall_breakdown_us:\n";
    print_stats("total", probe.samples.overall_total_ns);
    print_stats("stage_sum", probe.samples.overall_stage_sum_ns);
    print_stats("unattributed", probe.samples.overall_unattributed_ns);
    print_stats(
        "filter_to_state_dispatch",
        probe.samples.filter_to_state_dispatch_ns
    );
    print_stats(
        "state_to_signal_dispatch",
        probe.samples.state_to_signal_dispatch_ns
    );
    print_stats(
        "signal_to_risk_handoff_build",
        probe.samples.signal_to_risk_handoff_build_ns
    );
    print_stats(
        "risk_input_view_build",
        probe.samples.risk_input_view_build_ns
    );
    print_stats("order_decision", probe.samples.order_decision_ns);
    print_stats("fast_depth_read", probe.samples.fast_depth_read_ns);
    print_stats("fast_kernel", probe.samples.fast_kernel_ns);
    print_stats(
        "risk_context_requery",
        probe.samples.risk_context_requery_ns
    );
    print_stats(
        "risk_context_copy",
        probe.samples.risk_context_copy_ns
    );
    print_stats(
        "risk_to_execution_envelope_build",
        probe.samples.risk_to_execution_envelope_build_ns
    );
    print_stats(
        "execution_context_build",
        probe.samples.execution_context_build_ns
    );
    print_stats(
        "execution_submit_fill",
        probe.samples.execution_submit_fill_ns
    );
    print_stats(
        "unattributed_residual",
        probe.samples.overall_unattributed_residual_ns
    );

    std::cout << "\norder_decision_breakdown_us:\n";
    print_stats(
        "input_validate",
        probe.samples.order_decision_input_validate_ns
    );
    print_stats(
        "depth_view_prepare",
        probe.samples.order_decision_depth_view_prepare_ns
    );
    print_stats(
        "cost_curve_build",
        probe.samples.order_decision_cost_curve_build_ns
    );
    print_stats(
        "candidate_enumerate",
        probe.samples.order_decision_candidate_enumerate_ns
    );
    print_stats(
        "candidate_sort_dedup",
        probe.samples.order_decision_candidate_sort_dedup_ns
    );
    print_stats(
        "candidate_eval",
        probe.samples.order_decision_candidate_eval_ns
    );
    print_stats(
        "limit_price_build",
        probe.samples.order_decision_limit_price_build_ns
    );
    print_stats(
        "decision_build",
        probe.samples.order_decision_decision_build_ns
    );
    print_stats(
        "decision_hash",
        probe.samples.order_decision_decision_hash_ns
    );
    print_stats(
        "debug_materialize",
        probe.samples.order_decision_debug_materialize_ns
    );
    print_stats("publisher", probe.samples.order_decision_publisher_ns);
    print_stats("stage_sum", probe.samples.order_decision_stage_sum_ns);
    print_stats("unattributed", probe.samples.order_decision_unattributed_ns);

    std::cout << "\norder_decision_stats:\n";
    print_count_p50(
        "candidate_count_p50",
        probe.samples.order_decision_candidate_count
    );
    print_count_p50(
        "candidates_evaluated_p50",
        probe.samples.order_decision_candidates_evaluated
    );
    print_count_p50(
        "vwap_cost_calls_p50",
        probe.samples.order_decision_vwap_cost_calls
    );
    print_count_p50(
        "depth_levels_scanned_p50",
        probe.samples.order_decision_depth_levels_scanned
    );
    print_scaled_count_p50(
        "avg_depth_levels_scanned_p50",
        probe.samples.order_decision_avg_depth_levels_scanned_x1000,
        1000.0
    );
    print_count_p50(
        "max_depth_levels_scanned_p50",
        probe.samples.order_decision_max_depth_levels_scanned
    );
    print_count_p50(
        "linear_cost_calls_p50",
        probe.samples.order_decision_linear_cost_calls
    );
    print_count_p50(
        "prefix_cost_calls_p50",
        probe.samples.order_decision_prefix_cost_calls
    );
    print_count_p50(
        "prefix_linear_mismatch_p50",
        probe.samples.order_decision_prefix_linear_mismatch
    );
    print_count_p50(
        "fixed_shape_spec_cache_hits_p50",
        probe.samples.order_decision_fixed_shape_spec_cache_hits
    );
    print_count_p50(
        "fixed_shape_spec_cache_misses_p50",
        probe.samples.order_decision_fixed_shape_spec_cache_misses
    );
    print_count_p50(
        "candidate_set_cache_hits_p50",
        probe.samples.order_decision_candidate_set_cache_hits
    );
    print_count_p50(
        "candidate_set_cache_misses_p50",
        probe.samples.order_decision_candidate_set_cache_misses
    );
    print_count_p50(
        "leg_cost_cache_hits_p50",
        probe.samples.order_decision_leg_cost_cache_hits
    );
    print_count_p50(
        "leg_cost_cache_misses_p50",
        probe.samples.order_decision_leg_cost_cache_misses
    );
    print_count_p50(
        "decision_memo_cache_hits_p50",
        probe.samples.order_decision_decision_memo_cache_hits
    );
    print_count_p50(
        "decision_memo_cache_misses_p50",
        probe.samples.order_decision_decision_memo_cache_misses
    );
    print_count_p50(
        "incremental_eval_steps_p50",
        probe.samples.order_decision_incremental_eval_steps
    );
    print_count_p50(
        "incremental_leg_updates_p50",
        probe.samples.order_decision_incremental_leg_updates
    );
    print_count_p50(
        "incremental_level_advances_p50",
        probe.samples.order_decision_incremental_level_advances
    );
    std::cout << "  prefix_linear_mismatch_total: "
              << sum_values(
                     probe.samples.order_decision_prefix_linear_mismatch
                 )
              << '\n';
    print_count_p50(
        "rejected_by_unit_edge_p50",
        probe.samples.order_decision_rejected_by_unit_edge
    );
    print_count_p50(
        "rejected_by_total_edge_p50",
        probe.samples.order_decision_rejected_by_total_edge
    );
    print_count_p50(
        "rejected_by_bps_p50",
        probe.samples.order_decision_rejected_by_bps
    );

    std::cout << "\nsignal_breakdown_us:\n";
    print_stats("bundle_scan", probe.samples.signal_bundle_scan_ns);
    print_stats(
        "settlement_check",
        probe.samples.signal_settlement_check_ns
    );
    print_stats("snapshot_reader", probe.samples.signal_snapshot_reader_ns);
    print_stats(
        "snapshot_consistency_guard",
        probe.samples.signal_snapshot_consistency_guard_ns
    );
    print_stats(
        "price_vector_builder",
        probe.samples.signal_price_vector_builder_ns
    );
    print_stats("vwap_precheck", probe.samples.signal_vwap_precheck_ns);
    print_stats("edge_calculator", probe.samples.signal_edge_calculator_ns);
    print_stats("intent_builder", probe.samples.signal_intent_builder_ns);
    print_stats("dedupe", probe.samples.signal_dedupe_ns);
    print_stats("rate_limiter", probe.samples.signal_rate_limiter_ns);
    print_stats("publisher", probe.samples.signal_publisher_ns);

    std::cout << "\nrisk_stage_timings_us:\n";
    print_stats("kill_switch_guard", probe.samples.risk_kill_switch_guard_ns);
    print_stats("intent_validator", probe.samples.risk_intent_validator_ns);
    print_stats("evidence_verifier", probe.samples.risk_evidence_verifier_ns);
    print_stats("expiry_guard", probe.samples.risk_expiry_guard_ns);
    print_stats("duplicate_guard", probe.samples.risk_duplicate_guard_ns);
    print_stats("rate_limit_guard", probe.samples.risk_rate_limit_guard_ns);
    print_stats("market_state_guard", probe.samples.risk_market_state_guard_ns);
    print_stats(
        "snapshot_freshness_guard",
        probe.samples.risk_snapshot_freshness_guard_ns
    );
    print_stats("cost_revalidator", probe.samples.risk_cost_revalidator_ns);
    print_stats("vwap_revalidator", probe.samples.risk_vwap_revalidator_ns);
    print_stats("edge_guard", probe.samples.risk_edge_guard_ns);
    print_stats("max_loss_guard", probe.samples.risk_max_loss_guard_ns);
    print_stats("exposure_guard", probe.samples.risk_exposure_guard_ns);
    print_stats("inventory_guard", probe.samples.risk_inventory_guard_ns);
    print_stats("partial_fill_guard", probe.samples.risk_partial_fill_guard_ns);
    print_stats("reservation_book", probe.samples.risk_reservation_book_ns);
    print_stats("audit_trace", probe.samples.risk_audit_trace_ns);
    print_stats(
        "risk_decision_build",
        probe.samples.risk_decision_build_ns
    );
    print_stats("publisher", probe.samples.risk_publisher_ns);
    print_stats("metrics", probe.samples.risk_metrics_ns);

    std::cout << "\nrisk_breakdown_us:\n";
    print_stats("total", probe.samples.risk_total_ns);
    print_stats("stage_sum", probe.samples.risk_stage_sum_ns);
    print_stats("unattributed", probe.samples.risk_unattributed_ns);
    print_stats("cheap_guards", probe.samples.risk_cheap_guards_ns);
    print_stats(
        "snapshot_revalidation",
        probe.samples.risk_snapshot_revalidation_ns
    );
    print_stats("vwap_revalidation", probe.samples.risk_vwap_revalidation_ns);
    print_stats("edge_guard", probe.samples.risk_edge_guard_group_ns);
    print_stats(
        "exposure_inventory",
        probe.samples.risk_exposure_inventory_ns
    );
    print_stats("partial_fill", probe.samples.risk_partial_fill_ns);
    print_stats("reservation", probe.samples.risk_reservation_ns);
    print_stats("audit_trace", probe.samples.risk_audit_trace_ns);
    print_stats(
        "risk_decision_build",
        probe.samples.risk_decision_build_ns
    );
    print_stats("publisher", probe.samples.risk_publisher_ns);
    print_stats("metrics", probe.samples.risk_metrics_ns);
    print_stats("audit_publish", probe.samples.risk_audit_publish_ns);

    if (probe.tsc_available && probe.tsc_cycles_per_ns > 0.0) {
        std::vector<std::uint64_t> converted_ns;
        converted_ns.reserve(probe.samples.filter_to_plan_cycles.size());
        for (const auto cycles : probe.samples.filter_to_plan_cycles) {
            converted_ns.push_back(static_cast<std::uint64_t>(
                static_cast<double>(cycles) / probe.tsc_cycles_per_ns
            ));
        }
        std::cout << "\ntsc_converted_latency_us:\n";
        print_stats(
            "filter_pass_to_plan_total_from_cycles",
            converted_ns
        );
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto config = parse_args(argc, argv);
        ProbeState probe;
        probe.tsc_available = tsc_supported();
        probe.tsc_cycles_per_ns = calibrate_tsc_cycles_per_ns();
        run_probe(config, &probe);
        if (config.check_determinism) {
            ProbeState verifier;
            run_probe(config, &verifier);
            probe.determinism_checked = true;
            probe.determinism_passed =
                same_counters(probe.counters, verifier.counters);
        }
        print_report(config, probe);
        return probe.determinism_passed ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "probe_market_event_to_execution_plan_latency failed: "
                  << error.what() << '\n';
        return 1;
    }
}
