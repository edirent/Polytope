#include "tests/integration/signal/signal_workflow_test_utils.h"

#include <exception>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace signal_workflow_test;

CandidateBundle bundle_with_id(std::uint64_t bundle_id) {
    auto bundle = two_leg_bundle();
    bundle.bundle_id = bundle_id;
    return bundle;
}

std::vector<MarketStateSnapshot> positive_snapshots() {
    return {
        snapshot("asset_yes", 400'000, 10.0),
        snapshot("asset_no", 400'000, 10.0)
    };
}

void SignalEngine_PublishesOpportunityWithLifecycleFields() {
    EngineHarness harness({bundle_with_id(100)}, positive_snapshots());
    auto engine = harness.make_engine();

    const auto scan_context = context();
    const auto result = engine.scan_once(scan_context);

    expect_equal(result.paper_opportunities, 1ULL, "paper count");
    expect_equal(result.intents_published, 1ULL, "published count");
    expect_equal(result.vwap_checked, 1ULL, "vwap checked");
    expect_equal(result.edge_computed, 1ULL, "edge computed");
    expect_equal(harness.publisher.intents().size(), 1U, "captured count");

    const auto& intent = harness.publisher.intents()[0];
    expect_equal(intent.status, IntentStatus::PaperOpportunity, "status");
    expect_equal(intent.created_ts_ns, scan_context.now_monotonic_ns, "created");
    expect_equal(
        intent.expires_at_ns,
        scan_context.now_monotonic_ns + harness.config.intent_ttl_ns,
        "expires"
    );
    expect_true(intent.idempotency_hash != 0, "idempotency hash");
    expect_true(!intent.idempotency_key.empty(), "idempotency key");
    expect_true(!intent.proof_ref.empty(), "proof ref");
    expect_true(intent.oracle_artifact_hash != 0, "artifact hash");
    expect_true(intent.bundle_hash != 0, "bundle hash");
    expect_true(intent.snapshot_version_hash != 0, "snapshot hash");
}

void SignalEngine_DedupesRepeatedOpportunity() {
    EngineHarness harness({bundle_with_id(100)}, positive_snapshots());
    IntentDeduper deduper(1'000'000);
    auto engine = harness.make_engine(&deduper);

    const auto first = engine.scan_once(context());
    const auto first_key = harness.publisher.intents()[0].idempotency_key;
    const auto second = engine.scan_once(context());

    expect_equal(first.paper_opportunities, 1ULL, "first paper");
    expect_equal(first.intents_published, 1ULL, "first published");
    expect_equal(second.paper_opportunities, 1ULL, "second paper");
    expect_equal(second.rejected_duplicate, 1ULL, "rejected duplicate");
    expect_equal(second.duplicate_intents, 1ULL, "duplicate alias");
    expect_equal(second.intents_published, 0ULL, "second published");
    expect_equal(harness.publisher.intents().size(), 1U, "captured count");
    expect_equal(
        harness.publisher.intents()[0].idempotency_key,
        first_key,
        "stable key"
    );
}

void SignalEngine_RateLimitsManyOpportunities() {
    EngineHarness harness(
        {bundle_with_id(100), bundle_with_id(101)},
        positive_snapshots()
    );
    harness.config.max_intents_per_second = 1;
    auto engine = harness.make_engine();

    const auto result = engine.scan_once(context());

    expect_equal(result.paper_opportunities, 2ULL, "paper count");
    expect_equal(result.rejected_rate_limited, 1ULL, "rejected rate limited");
    expect_equal(result.rate_limited, 1ULL, "rate limited alias");
    expect_equal(result.intents_published, 1ULL, "published");
    expect_equal(harness.publisher.intents().size(), 1U, "captured count");
}

void SignalEngine_RejectsSnapshotVersionSkew() {
    auto yes = snapshot("asset_yes", 400'000, 10.0);
    auto no = snapshot("asset_no", 400'000, 10.0);
    yes.version = 1;
    no.version = 100;

    EngineHarness harness({bundle_with_id(100)}, {yes, no});
    harness.config.max_snapshot_version_skew = 10;
    auto engine = harness.make_engine();

    const auto result = engine.scan_once(context());

    expect_equal(result.rejected_bad_market_state, 1ULL, "bad state");
    expect_equal(result.rejected_snapshot_skew, 1ULL, "snapshot skew");
    expect_equal(result.vwap_checked, 0ULL, "vwap checked");
    expect_equal(result.edge_computed, 0ULL, "edge computed");
    expect_equal(result.paper_opportunities, 0ULL, "paper");
    expect_equal(result.intents_published, 1ULL, "published rejection");
    expect_equal(harness.publisher.intents()[0].status,
                 IntentStatus::RejectedBadMarketState,
                 "status");
}

void SignalEngine_ComputesBundleQtyAndTotalEdge() {
    EngineHarness harness({bundle_with_id(100)}, positive_snapshots());
    auto engine = harness.make_engine();

    const auto result = engine.scan_once(context());

    expect_equal(result.paper_opportunities, 1ULL, "paper count");
    const auto& intent = harness.publisher.intents()[0];
    expect_equal(intent.bundle_qty, 10LL, "bundle qty");
    expect_equal(intent.unit_edge_tick, 200'000LL, "unit edge");
    expect_equal(intent.total_edge_tick, 2'000'000LL, "total edge");
    expect_equal(intent.estimated_edge_tick, 2'000'000LL, "estimated edge");
    expect_equal(intent.estimated_cost_tick, 8'000'000LL, "total cost");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "SignalEngine_PublishesOpportunityWithLifecycleFields",
            &SignalEngine_PublishesOpportunityWithLifecycleFields
        },
        {
            "SignalEngine_DedupesRepeatedOpportunity",
            &SignalEngine_DedupesRepeatedOpportunity
        },
        {
            "SignalEngine_RateLimitsManyOpportunities",
            &SignalEngine_RateLimitsManyOpportunities
        },
        {
            "SignalEngine_RejectsSnapshotVersionSkew",
            &SignalEngine_RejectsSnapshotVersionSkew
        },
        {
            "SignalEngine_ComputesBundleQtyAndTotalEdge",
            &SignalEngine_ComputesBundleQtyAndTotalEdge
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
