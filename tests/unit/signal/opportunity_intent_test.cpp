#include "engine/signal/public/OpportunityIntent.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::signal::IntentStatus;
using trading_engine::signal::OpportunityIntent;

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

void OpportunityIntent_DefaultLifecycleFieldsSafe() {
    const OpportunityIntent intent;

    expect_equal(intent.status, IntentStatus::CandidateOnly, "status");
    expect_equal(intent.bundle_qty, 0LL, "bundle_qty");
    expect_equal(intent.original_bundle_qty, 0LL, "original_bundle_qty");
    expect_equal(intent.unit_edge_tick, 0LL, "unit_edge_tick");
    expect_equal(intent.total_edge_tick, 0LL, "total_edge_tick");
    expect_equal(intent.edge_bps, 0LL, "edge_bps");
    expect_equal(intent.slippage_buffer_tick, 0LL, "slippage_buffer_tick");
    expect_equal(intent.max_leg_slippage_tick, 0LL, "max_leg_slippage_tick");
    expect_equal(intent.created_ts_ns, 0ULL, "created_ts_ns");
    expect_equal(intent.expires_at_ns, 0ULL, "expires_at_ns");
}

void OpportunityIntent_HasArtifactHashes() {
    OpportunityIntent intent;
    intent.oracle_artifact_hash = 11;
    intent.constraint_hash = 22;
    intent.bundle_hash = 33;

    expect_equal(intent.oracle_artifact_hash, 11ULL, "oracle_artifact_hash");
    expect_equal(intent.constraint_hash, 22ULL, "constraint_hash");
    expect_equal(intent.bundle_hash, 33ULL, "bundle_hash");
}

void OpportunityIntent_HasSnapshotVersion() {
    OpportunityIntent intent;
    intent.snapshot_version = 44;
    intent.snapshot_version_hash = 55;

    expect_equal(intent.snapshot_version, 44ULL, "snapshot_version");
    expect_equal(intent.snapshot_version_hash, 55ULL, "snapshot_version_hash");
}

void OpportunityIntent_HasExpiry() {
    OpportunityIntent intent;
    intent.created_ts_ns = 100;
    intent.expires_at_ns = 250;

    expect_equal(intent.created_ts_ns, 100ULL, "created_ts_ns");
    expect_equal(intent.expires_at_ns, 250ULL, "expires_at_ns");
    expect_true(intent.expires_at_ns > intent.created_ts_ns, "expiry after created");
}

void OpportunityIntent_HasIdempotencyKey() {
    OpportunityIntent intent;
    intent.idempotency_key = "bundle-1:snapshot-2";
    intent.proof_ref = "oracle/artifact/proof";

    expect_equal(
        intent.idempotency_key,
        std::string{"bundle-1:snapshot-2"},
        "idempotency_key"
    );
    expect_equal(
        intent.proof_ref,
        std::string{"oracle/artifact/proof"},
        "proof_ref"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "OpportunityIntent_DefaultLifecycleFieldsSafe",
            &OpportunityIntent_DefaultLifecycleFieldsSafe
        },
        {
            "OpportunityIntent_HasArtifactHashes",
            &OpportunityIntent_HasArtifactHashes
        },
        {
            "OpportunityIntent_HasSnapshotVersion",
            &OpportunityIntent_HasSnapshotVersion
        },
        {"OpportunityIntent_HasExpiry", &OpportunityIntent_HasExpiry},
        {
            "OpportunityIntent_HasIdempotencyKey",
            &OpportunityIntent_HasIdempotencyKey
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
