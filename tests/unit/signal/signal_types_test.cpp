#include "engine/signal/public/OpportunityIntent.h"
#include "engine/signal/public/SignalConfig.h"
#include "engine/signal/public/SignalResult.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::signal::IntentStatus;
using trading_engine::signal::OpportunityIntent;
using trading_engine::signal::SignalConfig;
using trading_engine::signal::SignalMetrics;
using trading_engine::signal::SignalRunResult;
using trading_engine::signal::SnapshotConsistencyMode;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
    }
}

template <typename Actual, typename Expected>
void expect_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& field
) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
}

void SignalConfig_DefaultsAreSafe() {
    const SignalConfig config;

    expect_true(config.require_usable_for_depth, "require_usable_for_depth");
    expect_false(config.require_usable_for_signal, "require_usable_for_signal");
    expect_equal(config.default_fee_tick, 0LL, "default_fee_tick");
    expect_equal(
        config.default_latency_buffer_tick,
        0LL,
        "default_latency_buffer_tick"
    );
    expect_equal(config.slippage_buffer_tick, 0LL, "slippage_buffer_tick");
    expect_equal(config.min_unit_edge_tick, 0LL, "min_unit_edge_tick");
    expect_equal(config.min_total_edge_tick, 0LL, "min_total_edge_tick");
    expect_equal(config.min_edge_bps, 0LL, "min_edge_bps");
    expect_equal(config.min_bundle_qty, 1LL, "min_bundle_qty");
    expect_equal(config.max_intents_per_scan, 1024U, "max_intents_per_scan");
    expect_equal(
        config.max_intents_per_second,
        100,
        "max_intents_per_second"
    );
    expect_equal(config.max_bundle_legs, 16U, "max_bundle_legs");
    expect_equal(config.intent_ttl_ns, 5'000'000'000ULL, "intent_ttl_ns");
    expect_true(config.emit_rejections, "emit_rejections");
    expect_equal(config.max_lob_age_ns, 1'000'000'000LL, "max_lob_age_ns");
    expect_equal(
        config.max_snapshot_version_skew,
        10ULL,
        "max_snapshot_version_skew"
    );
    expect_equal(
        config.consistency_mode,
        SnapshotConsistencyMode::BoundedSkew,
        "consistency_mode"
    );
}

void OpportunityIntent_DefaultIsCandidateOnly() {
    const OpportunityIntent intent;

    expect_equal(intent.intent_id, 0ULL, "intent_id");
    expect_equal(intent.bundle_id, 0ULL, "bundle_id");
    expect_equal(
        intent.status,
        IntentStatus::CandidateOnly,
        "status"
    );
    expect_false(intent.valid_under_settlement, "valid_under_settlement");
    expect_false(intent.passed_quality_gate, "passed_quality_gate");
    expect_false(intent.enough_depth, "enough_depth");
    expect_equal(intent.estimated_edge_tick, 0LL, "estimated_edge_tick");
    expect_equal(intent.bundle_qty, 0LL, "bundle_qty");
    expect_equal(intent.original_bundle_qty, 0LL, "original_bundle_qty");
    expect_equal(intent.idempotency_hash, 0ULL, "idempotency_hash");
    expect_equal(intent.proof_hash, 0ULL, "proof_hash");
    expect_equal(
        intent.reject_code,
        trading_engine::signal::IntentRejectCode::None,
        "reject_code"
    );
    expect_equal(intent.unit_edge_tick, 0LL, "unit_edge_tick");
    expect_equal(intent.total_edge_tick, 0LL, "total_edge_tick");
    expect_equal(intent.edge_bps, 0LL, "edge_bps");
    expect_equal(intent.max_leg_slippage_tick, 0LL, "max_leg_slippage_tick");
    expect_equal(intent.leg_count, 0U, "leg_count");
    expect_equal(intent.snapshot_version_hash, 0ULL, "snapshot_version_hash");
    expect_equal(intent.oracle_artifact_version, 0ULL, "oracle_artifact_version");
    expect_true(intent.reject_reason.empty(), "reject_reason");
}

void SignalRunResult_DefaultCountersZero() {
    const SignalRunResult result;

    expect_equal(result.bundles_scanned, 0ULL, "bundles_scanned");
    expect_equal(
        result.rejected_invalid_settlement,
        0ULL,
        "rejected_invalid_settlement"
    );
    expect_equal(
        result.rejected_bad_market_state,
        0ULL,
        "rejected_bad_market_state"
    );
    expect_equal(
        result.rejected_missing_snapshot,
        0ULL,
        "rejected_missing_snapshot"
    );
    expect_equal(
        result.rejected_insufficient_depth,
        0ULL,
        "rejected_insufficient_depth"
    );
    expect_equal(result.rejected_low_edge, 0ULL, "rejected_low_edge");
    expect_equal(result.duplicate_intents, 0ULL, "duplicate_intents");
    expect_equal(result.rate_limited, 0ULL, "rate_limited");
    expect_equal(result.rejected_duplicate, 0ULL, "rejected_duplicate");
    expect_equal(result.rejected_rate_limited, 0ULL, "rejected_rate_limited");
    expect_equal(result.rejected_snapshot_skew, 0ULL, "rejected_snapshot_skew");
    expect_equal(result.rejected_stale_snapshot, 0ULL, "rejected_stale_snapshot");
    expect_equal(result.vwap_checked, 0ULL, "vwap_checked");
    expect_equal(result.edge_computed, 0ULL, "edge_computed");
    expect_equal(result.paper_opportunities, 0ULL, "paper_opportunities");
    expect_equal(result.intents_published, 0ULL, "intents_published");
    expect_equal(result.output_hash, 0ULL, "output_hash");
    expect_equal(
        result.stage_timings.bundle_scan_ns,
        0ULL,
        "stage_timings.bundle_scan_ns"
    );
    expect_equal(
        result.stage_timings.settlement_check_ns,
        0ULL,
        "stage_timings.settlement_check_ns"
    );
    expect_equal(
        result.stage_timings.snapshot_reader_ns,
        0ULL,
        "stage_timings.snapshot_reader_ns"
    );
    expect_equal(
        result.stage_timings.snapshot_consistency_guard_ns,
        0ULL,
        "stage_timings.snapshot_consistency_guard_ns"
    );
    expect_equal(
        result.stage_timings.price_vector_builder_ns,
        0ULL,
        "stage_timings.price_vector_builder_ns"
    );
    expect_equal(
        result.stage_timings.vwap_precheck_ns,
        0ULL,
        "stage_timings.vwap_precheck_ns"
    );
    expect_equal(
        result.stage_timings.edge_calculator_ns,
        0ULL,
        "stage_timings.edge_calculator_ns"
    );
    expect_equal(
        result.stage_timings.intent_builder_ns,
        0ULL,
        "stage_timings.intent_builder_ns"
    );
    expect_equal(
        result.stage_timings.dedupe_ns,
        0ULL,
        "stage_timings.dedupe_ns"
    );
    expect_equal(
        result.stage_timings.rate_limiter_ns,
        0ULL,
        "stage_timings.rate_limiter_ns"
    );
    expect_equal(
        result.stage_timings.publisher_ns,
        0ULL,
        "stage_timings.publisher_ns"
    );
    expect_equal(result.metrics.scan_count, 0ULL, "metrics.scan_count");
    expect_equal(
        result.metrics.reject_insufficient_depth,
        0ULL,
        "metrics.reject_insufficient_depth"
    );
    expect_equal(
        result.metrics.scan_latency_ns.count,
        0ULL,
        "metrics.scan_latency_ns.count"
    );
}

void SignalMetrics_DefaultCountersZero() {
    const SignalMetrics metrics;

    expect_equal(metrics.scan_count, 0ULL, "scan_count");
    expect_equal(metrics.bundle_scanned, 0ULL, "bundle_scanned");
    expect_equal(metrics.bundle_rejected, 0ULL, "bundle_rejected");
    expect_equal(metrics.bundle_passed, 0ULL, "bundle_passed");
    expect_equal(metrics.reject_settled, 0ULL, "reject_settled");
    expect_equal(
        metrics.reject_missing_snapshot,
        0ULL,
        "reject_missing_snapshot"
    );
    expect_equal(metrics.reject_stale_lob, 0ULL, "reject_stale_lob");
    expect_equal(
        metrics.reject_snapshot_skew,
        0ULL,
        "reject_snapshot_skew"
    );
    expect_equal(
        metrics.reject_insufficient_depth,
        0ULL,
        "reject_insufficient_depth"
    );
    expect_equal(
        metrics.reject_edge_below_threshold,
        0ULL,
        "reject_edge_below_threshold"
    );
    expect_equal(metrics.reject_duplicate, 0ULL, "reject_duplicate");
    expect_equal(metrics.reject_rate_limited, 0ULL, "reject_rate_limited");
    expect_equal(metrics.intent_published, 0ULL, "intent_published");
    expect_equal(metrics.scan_latency_ns.count, 0ULL, "latency count");
    expect_equal(metrics.scan_latency_ns.last_ns, 0ULL, "latency last");
}

void SignalMetrics_ObserveScanLatency() {
    SignalMetrics metrics;

    metrics.observe_scan_latency(100);
    metrics.observe_scan_latency(40);
    metrics.observe_scan_latency(250);

    expect_equal(metrics.scan_latency_ns.count, 3ULL, "latency count");
    expect_equal(metrics.scan_latency_ns.last_ns, 250ULL, "latency last");
    expect_equal(metrics.scan_latency_ns.min_ns, 40ULL, "latency min");
    expect_equal(metrics.scan_latency_ns.max_ns, 250ULL, "latency max");
    expect_equal(metrics.scan_latency_ns.total_ns, 390ULL, "latency total");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"SignalConfig_DefaultsAreSafe", &SignalConfig_DefaultsAreSafe},
        {
            "OpportunityIntent_DefaultIsCandidateOnly",
            &OpportunityIntent_DefaultIsCandidateOnly
        },
        {
            "SignalRunResult_DefaultCountersZero",
            &SignalRunResult_DefaultCountersZero
        },
        {
            "SignalMetrics_DefaultCountersZero",
            &SignalMetrics_DefaultCountersZero
        },
        {
            "SignalMetrics_ObserveScanLatency",
            &SignalMetrics_ObserveScanLatency
        }
    };
    return test_map;
}

int run_test(const std::string& name) {
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << name << " failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << name << " passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        return run_test(argv[1]);
    }

    int failures = 0;
    for (const auto& [name, _] : tests()) {
        failures += run_test(name) == 0 ? 0 : 1;
    }
    return failures == 0 ? 0 : 1;
}
