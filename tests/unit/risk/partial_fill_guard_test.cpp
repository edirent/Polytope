#include "engine/risk/guards/PartialFillGuard.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::risk::CostRevalidationResult;
using trading_engine::risk::PartialFillGuard;
using trading_engine::risk::RiskDecisionType;
using trading_engine::risk::RiskPolicySnapshot;
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

OpportunityIntent make_intent(std::uint16_t leg_count) {
    OpportunityIntent intent;
    intent.intent_id = 1;
    intent.bundle_id = 2;
    intent.status = IntentStatus::PaperOpportunity;
    intent.bundle_qty = 100;
    intent.expires_at_ns = 2'000;
    intent.idempotency_key = "partial-fill";
    intent.leg_count = leg_count;
    for (std::uint16_t i = 0; i < leg_count; ++i) {
        auto& leg = intent.legs[i];
        leg.market_id = "market-" + std::to_string(i);
        leg.asset_id = "asset-" + std::to_string(i);
        leg.quantity_lots = 10;
        leg.estimated_cost_tick = 100;
    }
    return intent;
}

CostRevalidationResult cost(
    std::int64_t risk_bundle_qty,
    std::uint16_t leg_count,
    std::int64_t requested_qty_lots,
    std::int64_t executable_qty_lots
) {
    CostRevalidationResult out;
    out.ok = true;
    out.risk_bundle_qty = risk_bundle_qty;
    out.risk_total_cost_tick = 100;
    out.rejection = RiskDecisionType::Approve;
    out.leg_count = leg_count;
    for (std::uint16_t i = 0; i < leg_count; ++i) {
        auto& leg = out.legs[i];
        leg.asset_id = "asset-" + std::to_string(i);
        leg.requested_qty_lots = requested_qty_lots;
        leg.executable_qty_lots = executable_qty_lots;
        leg.depth_margin_bps =
            requested_qty_lots > 0
                ? executable_qty_lots * 10'000 / requested_qty_lots
                : 0;
        leg.enough_depth = executable_qty_lots >= requested_qty_lots;
    }
    return out;
}

void PartialFillGuard_AllowsSingleLeg() {
    RiskPolicySnapshot policy;
    policy.min_depth_margin_ratio = 2.0;
    policy.max_unhedged_loss_tick = 123;

    const auto result = PartialFillGuard{}.check(
        make_intent(1),
        cost(1, 1, 10, 10),
        policy
    );

    expect_true(result.pass, "pass");
    expect_equal(result.rejection, RiskDecisionType::Approve, "rejection");
    expect_false(result.depth_margin_checked, "single leg depth margin");
    expect_false(result.unhedged_loss_evaluated, "unhedged evaluated");
    expect_true(
        result.unhedged_loss_placeholder_available,
        "unhedged placeholder"
    );
}

void PartialFillGuard_RejectsMultiLegBarelyEnoughDepth() {
    RiskPolicySnapshot policy;
    policy.min_depth_margin_ratio = 1.20;
    policy.min_depth_margin_bps = 12'000;

    const auto result = PartialFillGuard{}.check(
        make_intent(2),
        cost(1, 2, 10, 10),
        policy
    );

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectPartialFillRisk,
        "rejection"
    );
    expect_equal(result.requested_qty_lots, 10LL, "requested");
    expect_equal(result.available_depth_lots, 10LL, "available");
    expect_equal(
        result.required_depth_with_margin_lots,
        12LL,
        "required"
    );
}

void PartialFillGuard_UsesReservationQty() {
    RiskPolicySnapshot policy;
    policy.min_depth_margin_ratio = 1.20;
    policy.min_depth_margin_bps = 12'000;

    const auto result = PartialFillGuard{}.check(
        make_intent(2),
        cost(5, 2, 50, 60),
        policy
    );

    expect_true(result.pass, "pass");
    expect_equal(result.rejection, RiskDecisionType::Approve, "rejection");
    expect_true(result.depth_margin_checked, "depth margin checked");
}

void PartialFillGuard_InterfaceHasUnhedgedLossPlaceholder() {
    RiskPolicySnapshot policy;
    policy.min_depth_margin_ratio = 1.0;
    policy.min_depth_margin_bps = 10'000;
    policy.max_unhedged_loss_tick = 777;

    const auto result = PartialFillGuard{}.check(
        make_intent(2),
        cost(1, 2, 10, 10),
        policy
    );

    expect_true(result.pass, "pass");
    expect_false(result.unhedged_loss_evaluated, "unhedged evaluated");
    expect_true(
        result.unhedged_loss_placeholder_available,
        "unhedged placeholder"
    );
    expect_equal(result.max_unhedged_loss_tick, 777LL, "max unhedged loss");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"PartialFillGuard_AllowsSingleLeg", &PartialFillGuard_AllowsSingleLeg},
        {"PartialFillGuard_RejectsMultiLegBarelyEnoughDepth", &PartialFillGuard_RejectsMultiLegBarelyEnoughDepth},
        {"PartialFillGuard_UsesReservationQty", &PartialFillGuard_UsesReservationQty},
        {"PartialFillGuard_InterfaceHasUnhedgedLossPlaceholder", &PartialFillGuard_InterfaceHasUnhedgedLossPlaceholder}
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
