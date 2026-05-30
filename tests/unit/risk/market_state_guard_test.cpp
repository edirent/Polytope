#include "engine/risk/guards/MarketStateGuard.h"
#include "engine/risk/guards/SnapshotFreshnessGuard.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::risk::MarketStateGuard;
using trading_engine::risk::RiskDecisionType;
using trading_engine::risk::RiskPolicySnapshot;
using trading_engine::risk::SnapshotFreshnessGuard;
using trading_engine::state::MarketStateSnapshot;
using trading_engine::signal::OpportunityIntent;

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

MarketStateSnapshot healthy_snapshot() {
    MarketStateSnapshot snapshot;
    snapshot.entity_id = "asset-1";
    snapshot.market_id = "market-1";
    snapshot.version = 10;
    snapshot.last_book_update_ns = 1'000;
    snapshot.live = true;
    snapshot.usable_for_depth = true;
    snapshot.state_hash = 12345;
    return snapshot;
}

OpportunityIntent intent_for(const MarketStateSnapshot& snapshot) {
    OpportunityIntent intent;
    intent.intent_id = 1;
    intent.bundle_id = 2;
    intent.snapshot_version = snapshot.version;
    intent.snapshot_version_hash = snapshot.state_hash;
    intent.created_ts_ns = 900;
    intent.expires_at_ns = 2'000;
    return intent;
}

void MarketStateGuard_RejectsRecovering() {
    auto snapshot = healthy_snapshot();
    snapshot.recovering = true;

    const auto result = MarketStateGuard{}.check(&snapshot);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectBadMarketState,
        "rejection"
    );
}

void MarketStateGuard_RejectsCrossed() {
    auto snapshot = healthy_snapshot();
    snapshot.crossed = true;

    const auto result = MarketStateGuard{}.check(&snapshot);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectBadMarketState,
        "rejection"
    );
}

void MarketStateGuard_RejectsClosed() {
    auto snapshot = healthy_snapshot();
    snapshot.closed = true;

    const auto result = MarketStateGuard{}.check(&snapshot);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectBadMarketState,
        "rejection"
    );
}

void MarketStateGuard_RejectsMissingSnapshot() {
    const auto result = MarketStateGuard{}.check(nullptr);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectBadMarketState,
        "rejection"
    );
}

void MarketStateGuard_RejectsNotUsableForDepth() {
    auto snapshot = healthy_snapshot();
    snapshot.usable_for_depth = false;

    const auto result = MarketStateGuard{}.check(&snapshot);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectBadMarketState,
        "rejection"
    );
}

void MarketStateGuard_RejectsResolved() {
    auto snapshot = healthy_snapshot();
    snapshot.resolved = true;

    const auto result = MarketStateGuard{}.check(&snapshot);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectBadMarketState,
        "rejection"
    );
}

void MarketStateGuard_RejectsHalted() {
    auto snapshot = healthy_snapshot();
    snapshot.live = false;

    const auto result = MarketStateGuard{}.check(&snapshot);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectBadMarketState,
        "rejection"
    );
}

void SnapshotFreshnessGuard_RejectsTooOld() {
    const auto snapshot = healthy_snapshot();
    const auto intent = intent_for(snapshot);
    RiskPolicySnapshot policy;
    policy.max_book_age_ns = 100;

    const auto result =
        SnapshotFreshnessGuard{}.check(snapshot, intent, policy, 1'200);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectStaleSnapshot,
        "rejection"
    );
}

void SnapshotFreshnessGuard_AllowsFreshDifferentVersionButRequiresReprice() {
    auto snapshot = healthy_snapshot();
    auto intent = intent_for(snapshot);
    snapshot.version = 11;
    snapshot.state_hash = 67890;

    RiskPolicySnapshot policy;
    policy.max_book_age_ns = 1'000;
    policy.max_snapshot_skew_ns = 10;

    const auto result =
        SnapshotFreshnessGuard{}.check(snapshot, intent, policy, 1'100);

    expect_true(result.pass, "pass");
    expect_true(result.requires_reprice, "requires_reprice");
    expect_equal(result.rejection, RiskDecisionType::Pass, "rejection");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "MarketStateGuard_RejectsRecovering",
            &MarketStateGuard_RejectsRecovering
        },
        {"MarketStateGuard_RejectsCrossed", &MarketStateGuard_RejectsCrossed},
        {"MarketStateGuard_RejectsClosed", &MarketStateGuard_RejectsClosed},
        {
            "MarketStateGuard_RejectsMissingSnapshot",
            &MarketStateGuard_RejectsMissingSnapshot
        },
        {
            "MarketStateGuard_RejectsNotUsableForDepth",
            &MarketStateGuard_RejectsNotUsableForDepth
        },
        {"MarketStateGuard_RejectsResolved", &MarketStateGuard_RejectsResolved},
        {"MarketStateGuard_RejectsHalted", &MarketStateGuard_RejectsHalted},
        {
            "SnapshotFreshnessGuard_RejectsTooOld",
            &SnapshotFreshnessGuard_RejectsTooOld
        },
        {
            "SnapshotFreshnessGuard_AllowsFreshDifferentVersionButRequiresReprice",
            &SnapshotFreshnessGuard_AllowsFreshDifferentVersionButRequiresReprice
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
