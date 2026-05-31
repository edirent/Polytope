#include "engine/signal/publish/IntentBuilder.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::BundleLeg;
using trading_engine::oracle::CandidateBundle;
using trading_engine::oracle::Side;
using trading_engine::signal::CostResult;
using trading_engine::signal::EdgeBreakdown;
using trading_engine::signal::FillSimulationLeg;
using trading_engine::signal::IntentBuildInput;
using trading_engine::signal::IntentBuilder;
using trading_engine::signal::IntentStatus;
using trading_engine::signal::materialize_intent_strings;
using trading_engine::signal::SnapshotReadResult;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
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

CandidateBundle bundle() {
    CandidateBundle out;
    out.bundle_id = 42;
    out.guaranteed_payout_tick = 1'000'000;
    out.min_edge_tick = 10'000;
    out.leg_count = 1;
    out.legs[0] = BundleLeg{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .side = Side::Buy,
        .quantity_lots = 1,
        .max_price_tick = 900'000
    };
    return out;
}

SnapshotReadResult snapshot() {
    SnapshotReadResult out;
    out.ok = true;
    out.snapshot_version.max_book_version = 77;
    out.snapshot_version.combined_hash = 0xabc;
    return out;
}

CostResult cost() {
    CostResult out;
    out.executable = true;
    out.bundle_qty = 5;
    out.total_cost_tick = 4'000'000;
    out.avg_cost_tick = 800'000;
    out.max_leg_slippage_tick = 10'000;
    out.legs.push_back(FillSimulationLeg{
        .asset_id = "asset_yes",
        .requested_qty_lots = 1,
        .executable_qty_lots = 5,
        .vwap_price_tick = 800'000,
        .total_cost_tick = 4'000'000,
        .worst_price_tick = 810'000,
        .book_age_ns = 100,
        .enough_depth = true
    });
    return out;
}

EdgeBreakdown edge() {
    EdgeBreakdown out;
    out.passed = true;
    out.guaranteed_payout_per_bundle_tick = 1'000'000;
    out.vwap_cost_per_bundle_tick = 800'000;
    out.fee_per_bundle_tick = 1'000;
    out.latency_buffer_per_bundle_tick = 2'000;
    out.slippage_buffer_per_bundle_tick = 3'000;
    out.unit_edge_tick = 194'000;
    out.total_edge_tick = 970'000;
    out.edge_bps = 2'425;
    out.bundle_qty = 5;
    return out;
}

IntentBuildInput input() {
    static const CandidateBundle b = bundle();
    static const SnapshotReadResult s = snapshot();
    static const CostResult c = cost();
    static const EdgeBreakdown e = edge();

    return IntentBuildInput{
        .bundle = &b,
        .snapshot = &s,
        .cost = &c,
        .edge = &e,
        .now_ns = 100,
        .ttl_ns = 900,
        .oracle_artifact_version = 3,
        .oracle_artifact_hash = 11,
        .constraint_hash = 22,
        .bundle_hash = 33,
        .valid_under_settlement = true,
        .passed_quality_gate = true
    };
}

void IntentBuilder_FillsLifecycleFields() {
    const auto intent = IntentBuilder{}.build(input());

    expect_equal(intent.status, IntentStatus::PaperOpportunity, "status");
    expect_equal(intent.created_ts_ns, 100ULL, "created");
    expect_equal(intent.expires_at_ns, 1'000ULL, "expires");
    expect_true(intent.intent_id != 0, "intent id");
}

void IntentBuilder_FillsCostEvidenceFields() {
    const auto intent = IntentBuilder{}.build(input());

    expect_equal(intent.bundle_qty, 5LL, "bundle qty");
    expect_equal(intent.original_bundle_qty, 5LL, "original bundle qty");
    expect_equal(intent.max_leg_slippage_tick, 10'000LL, "max slippage");
}

void IntentBuilder_FillsArtifactHashes() {
    const auto intent = IntentBuilder{}.build(input());

    expect_equal(intent.oracle_artifact_version, 3ULL, "artifact version");
    expect_equal(intent.oracle_artifact_hash, 11ULL, "artifact hash");
    expect_equal(intent.constraint_hash, 22ULL, "constraint hash");
    expect_equal(intent.bundle_hash, 33ULL, "bundle hash");
    expect_true(intent.proof_hash != 0, "proof hash");
}

void IntentBuilder_FillsSnapshotVersion() {
    const auto intent = IntentBuilder{}.build(input());

    expect_equal(intent.snapshot_version, 77ULL, "snapshot version");
    expect_equal(intent.snapshot_version_hash, 0xabcULL, "snapshot hash");
}

void IntentBuilder_GeneratesStableIdempotencyKey() {
    auto first = IntentBuilder{}.build(input());
    auto second = IntentBuilder{}.build(input());

    expect_true(first.idempotency_hash != 0, "idempotency hash");
    expect_equal(
        first.idempotency_hash,
        second.idempotency_hash,
        "idempotency hash stable"
    );
    materialize_intent_strings(&first);
    materialize_intent_strings(&second);
    expect_true(!first.idempotency_key.empty(), "idempotency key");
    expect_equal(
        first.idempotency_key,
        second.idempotency_key,
        "idempotency stable"
    );
    expect_equal(first.intent_id, second.intent_id, "intent id stable");
}

void IntentBuilder_ExpiresAtCreatedPlusTtl() {
    auto build_input = input();
    build_input.now_ns = 1'000;
    build_input.ttl_ns = 2'500;

    const auto intent = IntentBuilder{}.build(build_input);

    expect_equal(intent.created_ts_ns, 1'000ULL, "created");
    expect_equal(intent.expires_at_ns, 3'500ULL, "expires");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "IntentBuilder_FillsLifecycleFields",
            &IntentBuilder_FillsLifecycleFields
        },
        {
            "IntentBuilder_FillsCostEvidenceFields",
            &IntentBuilder_FillsCostEvidenceFields
        },
        {
            "IntentBuilder_FillsArtifactHashes",
            &IntentBuilder_FillsArtifactHashes
        },
        {
            "IntentBuilder_FillsSnapshotVersion",
            &IntentBuilder_FillsSnapshotVersion
        },
        {
            "IntentBuilder_GeneratesStableIdempotencyKey",
            &IntentBuilder_GeneratesStableIdempotencyKey
        },
        {
            "IntentBuilder_ExpiresAtCreatedPlusTtl",
            &IntentBuilder_ExpiresAtCreatedPlusTtl
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

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }
    return run_test(argv[1]);
}
