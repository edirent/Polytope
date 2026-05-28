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
using trading_engine::signal::SignalRunResult;

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
    expect_equal(config.max_intents_per_scan, 1024U, "max_intents_per_scan");
    expect_equal(config.max_bundle_legs, 16U, "max_bundle_legs");
    expect_true(config.emit_rejections, "emit_rejections");
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
    expect_equal(result.paper_opportunities, 0ULL, "paper_opportunities");
    expect_equal(result.intents_published, 0ULL, "intents_published");
    expect_equal(result.output_hash, 0ULL, "output_hash");
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
