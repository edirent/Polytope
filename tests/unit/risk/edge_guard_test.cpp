#include "engine/risk/guards/EdgeGuard.h"
#include "engine/risk/guards/MaxLossGuard.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::risk::CostRevalidationResult;
using trading_engine::risk::EdgeGuard;
using trading_engine::risk::MaxLossGuard;
using trading_engine::risk::RiskDecisionType;
using trading_engine::risk::RiskPolicySnapshot;
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

OpportunityIntent intent() {
    OpportunityIntent out;
    out.intent_id = 1;
    out.bundle_id = 2;
    out.bundle_qty = 10;
    // Signal stores this as total payout for its original bundle quantity.
    out.guaranteed_payout_tick = 10'000;
    out.leg_count = 1;
    out.legs[0].market_id = "market-1";
    out.legs[0].asset_id = "asset-1";
    out.legs[0].quantity_lots = 1;
    return out;
}

CostRevalidationResult cost() {
    CostRevalidationResult out;
    out.ok = true;
    out.risk_bundle_qty = 10;
    out.risk_total_cost_tick = 8'000;
    out.fee_tick = 100;
    out.slippage_buffer_tick = 100;
    out.latency_buffer_tick = 100;
    out.rejection = RiskDecisionType::Approve;
    return out;
}

void EdgeGuard_ApprovesAboveThreshold() {
    RiskPolicySnapshot policy;
    policy.min_post_risk_total_edge_tick = 1'000;
    policy.min_post_risk_unit_edge_tick = 100;
    policy.min_edge_bps = 1'000;

    const auto result = EdgeGuard{}.check(intent(), cost(), policy);

    expect_true(result.pass, "pass");
    expect_equal(result.rejection, RiskDecisionType::Approve, "rejection");
    expect_equal(result.post_risk_edge_tick, 1'700LL, "total edge");
    expect_equal(result.unit_edge_tick, 170LL, "unit edge");
}

void EdgeGuard_RejectsLowTotalEdge() {
    RiskPolicySnapshot policy;
    policy.min_post_risk_total_edge_tick = 2'000;

    const auto result = EdgeGuard{}.check(intent(), cost(), policy);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectLowTotalEdge,
        "rejection"
    );
}

void EdgeGuard_RejectsLowUnitEdge() {
    RiskPolicySnapshot policy;
    policy.min_post_risk_unit_edge_tick = 200;

    const auto result = EdgeGuard{}.check(intent(), cost(), policy);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectLowUnitEdge,
        "rejection"
    );
}

void EdgeGuard_RejectsLowBps() {
    auto c = cost();
    c.risk_total_cost_tick = 9'900;
    c.fee_tick = 0;
    c.slippage_buffer_tick = 0;
    c.latency_buffer_tick = 0;

    RiskPolicySnapshot policy;
    policy.min_edge_bps = 200;

    const auto result = EdgeGuard{}.check(intent(), c, policy);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectLowEdgeBps,
        "rejection"
    );
}

void MaxLossGuard_RejectsCostAboveLimit() {
    RiskPolicySnapshot policy;
    policy.max_total_cost_tick = 5'000;

    const auto result = MaxLossGuard{}.check(intent(), cost(), policy);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectCostLimit,
        "rejection"
    );
    expect_equal(result.max_loss_tick, 8'000LL, "max loss");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"EdgeGuard_ApprovesAboveThreshold", &EdgeGuard_ApprovesAboveThreshold},
        {"EdgeGuard_RejectsLowTotalEdge", &EdgeGuard_RejectsLowTotalEdge},
        {"EdgeGuard_RejectsLowUnitEdge", &EdgeGuard_RejectsLowUnitEdge},
        {"EdgeGuard_RejectsLowBps", &EdgeGuard_RejectsLowBps},
        {"MaxLossGuard_RejectsCostAboveLimit", &MaxLossGuard_RejectsCostAboveLimit}
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
