#include "engine/risk/reprice/CostRevalidator.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::risk::CostRevalidator;
using trading_engine::risk::RiskDecisionType;
using trading_engine::risk::RiskPolicySnapshot;
using trading_engine::signal::OpportunityIntent;
using trading_engine::state::MarketStateSnapshot;

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

MarketStateSnapshot snapshot(
    std::string asset_id,
    std::int64_t ask_price_tick,
    double ask_size
) {
    MarketStateSnapshot out;
    out.entity_id = std::move(asset_id);
    out.market_id = "market-1";
    out.version = 10;
    out.last_book_update_ns = 1'000;
    out.live = true;
    out.usable_for_depth = true;
    out.has_ask = true;
    out.ask_count = 1;
    out.asks[0].price_tick = ask_price_tick;
    out.asks[0].size = ask_size;
    return out;
}

OpportunityIntent intent() {
    OpportunityIntent out;
    out.intent_id = 1;
    out.bundle_id = 2;
    out.bundle_qty = 10;
    out.estimated_cost_tick = 1'000;
    out.estimated_fee_tick = 3;
    out.slippage_buffer_tick = 4;
    out.latency_buffer_tick = 5;
    out.leg_count = 1;
    out.legs[0].market_id = "market-1";
    out.legs[0].asset_id = "asset-1";
    out.legs[0].quantity_lots = 1;
    return out;
}

RiskPolicySnapshot permissive_policy() {
    RiskPolicySnapshot policy;
    policy.max_allowed_cost_drift_tick = 10'000;
    return policy;
}

void CostRevalidator_RecomputesVWAPFromLatestSnapshot() {
    auto in = intent();
    in.estimated_cost_tick = 1'000;
    const std::vector<MarketStateSnapshot> snapshots{
        snapshot("asset-1", 120, 10.0)
    };

    const auto result =
        CostRevalidator{}.revalidate(in, snapshots, permissive_policy());

    expect_true(result.ok, "ok");
    expect_equal(result.risk_total_cost_tick, 1'200LL, "risk cost");
    expect_equal(result.cost_drift_tick, 200LL, "cost drift");
    expect_equal(result.risk_bundle_qty, 10LL, "bundle qty");
    expect_equal(result.fee_tick, 3LL, "fee");
    expect_equal(result.slippage_buffer_tick, 4LL, "slippage");
    expect_equal(result.latency_buffer_tick, 5LL, "latency");
}

void CostRevalidator_RejectsInsufficientDepth() {
    auto bad_snapshot = snapshot("asset-1", 120, 0.0);
    bad_snapshot.has_ask = false;
    bad_snapshot.ask_count = 0;

    const auto result = CostRevalidator{}.revalidate(
        intent(),
        std::vector<MarketStateSnapshot>{bad_snapshot},
        permissive_policy()
    );

    expect_false(result.ok, "ok");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectInsufficientDepth,
        "rejection"
    );
}

void CostRevalidator_RejectsCostDriftTooHigh() {
    RiskPolicySnapshot policy;
    policy.max_allowed_cost_drift_tick = 50;

    const auto result = CostRevalidator{}.revalidate(
        intent(),
        std::vector<MarketStateSnapshot>{snapshot("asset-1", 120, 10.0)},
        policy
    );

    expect_false(result.ok, "ok");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectCostDrift,
        "rejection"
    );
    expect_equal(result.cost_drift_tick, 200LL, "drift");
}

void CostRevalidator_AllowsSmallCostDrift() {
    RiskPolicySnapshot policy;
    policy.max_allowed_cost_drift_tick = 250;

    const auto result = CostRevalidator{}.revalidate(
        intent(),
        std::vector<MarketStateSnapshot>{snapshot("asset-1", 120, 10.0)},
        policy
    );

    expect_true(result.ok, "ok");
    expect_equal(result.rejection, RiskDecisionType::Approve, "rejection");
    expect_equal(result.cost_drift_tick, 200LL, "drift");
}

void CostRevalidator_RejectsReducedBundleQty() {
    const auto result = CostRevalidator{}.revalidate(
        intent(),
        std::vector<MarketStateSnapshot>{snapshot("asset-1", 100, 9.0)},
        permissive_policy()
    );

    expect_false(result.ok, "ok");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectReducedBundleQty,
        "rejection"
    );
    expect_equal(result.risk_bundle_qty, 9LL, "risk bundle qty");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "CostRevalidator_RecomputesVWAPFromLatestSnapshot",
            &CostRevalidator_RecomputesVWAPFromLatestSnapshot
        },
        {
            "CostRevalidator_RejectsInsufficientDepth",
            &CostRevalidator_RejectsInsufficientDepth
        },
        {
            "CostRevalidator_RejectsCostDriftTooHigh",
            &CostRevalidator_RejectsCostDriftTooHigh
        },
        {
            "CostRevalidator_AllowsSmallCostDrift",
            &CostRevalidator_AllowsSmallCostDrift
        },
        {
            "CostRevalidator_RejectsReducedBundleQty",
            &CostRevalidator_RejectsReducedBundleQty
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
