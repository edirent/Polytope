#include "engine/order_decision/sizing/BundleSizeOptimizer.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::order_decision::BundleSizeInput;
using trading_engine::order_decision::BundleSizeOptimizer;
using trading_engine::order_decision::OrderDecisionConfig;
using trading_engine::order_decision::OrderDecisionType;
using trading_engine::oracle::BundleLeg;
using trading_engine::oracle::CandidateBundle;
using trading_engine::oracle::Side;
using trading_engine::risk::RiskPolicySnapshot;
using trading_engine::risk::with_computed_policy_hash;
using trading_engine::signal::IntentStatus;
using trading_engine::signal::OpportunityIntent;
using trading_engine::state::MarketDepthView;
using trading_engine::state::PriceLevel;

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

CandidateBundle bundle(std::int64_t guaranteed = 1'000'000) {
    CandidateBundle out;
    out.bundle_id = 10;
    out.guaranteed_payout_tick = guaranteed;
    out.leg_count = 1;
    out.legs[0] = BundleLeg{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .side = Side::Buy,
        .quantity_lots = 1,
        .max_price_tick = 1'000'000
    };
    return out;
}

OpportunityIntent intent() {
    OpportunityIntent out;
    out.intent_id = 42;
    out.bundle_id = 10;
    out.status = IntentStatus::PaperOpportunity;
    out.guaranteed_payout_tick = 1'000'000;
    out.leg_count = 1;
    out.legs[0].market_id = "m1";
    out.legs[0].asset_id = "asset_yes";
    out.legs[0].asset_index = 7;
    out.legs[0].side = Side::Buy;
    out.legs[0].quantity_lots = 1;
    return out;
}

MarketDepthView depth(std::initializer_list<PriceLevel> asks) {
    MarketDepthView out;
    out.asset_index = 7;
    out.usable_for_depth = true;
    for (const auto& ask : asks) {
        out.asks[out.ask_count++] = ask;
    }
    return out;
}

RiskPolicySnapshot policy() {
    RiskPolicySnapshot out;
    out.max_total_cost_tick = 100'000'000;
    out.min_depth_margin_bps = 10'000;
    return with_computed_policy_hash(out);
}

void Optimizer_PicksMaxTotalEdge() {
    const auto b = bundle();
    const auto i = intent();
    const auto d = depth({
        PriceLevel{.price_tick = 100'000, .price = 0.1, .size = 10.0},
        PriceLevel{.price_tick = 200'000, .price = 0.2, .size = 10.0}
    });
    const auto p = policy();

    const auto result = BundleSizeOptimizer{}.optimize({
        .intent = &i,
        .bundle = &b,
        .depth_views = std::span<const MarketDepthView>(&d, 1),
        .policy = &p
    });

    expect_true(result.ok, "ok");
    expect_equal(result.best_bundle_qty, 20LL, "qty");
    expect_equal(result.total_edge_tick, 17'000'000LL, "edge");
}

void Optimizer_RejectsLowUnitEdge() {
    const auto b = bundle(150'000);
    const auto i = intent();
    const auto d = depth({
        PriceLevel{.price_tick = 100'000, .price = 0.1, .size = 10.0}
    });
    auto p = policy();
    p.min_post_risk_unit_edge_tick = 60'000;
    p = with_computed_policy_hash(p);

    const auto result = BundleSizeOptimizer{}.optimize({
        .intent = &i,
        .bundle = &b,
        .depth_views = std::span<const MarketDepthView>(&d, 1),
        .policy = &p
    });

    expect_false(result.ok, "ok");
    expect_equal(result.reject_reason, OrderDecisionType::RejectLowEdge, "reason");
}

void Optimizer_RejectsLowTotalEdge() {
    const auto b = bundle();
    const auto i = intent();
    const auto d = depth({
        PriceLevel{.price_tick = 100'000, .price = 0.1, .size = 10.0}
    });
    auto p = policy();
    p.min_post_risk_total_edge_tick = 100'000'000;
    p = with_computed_policy_hash(p);

    const auto result = BundleSizeOptimizer{}.optimize({
        .intent = &i,
        .bundle = &b,
        .depth_views = std::span<const MarketDepthView>(&d, 1),
        .policy = &p
    });

    expect_false(result.ok, "ok");
    expect_equal(result.reject_reason, OrderDecisionType::RejectLowEdge, "reason");
}

void Optimizer_RejectsInsufficientDepth() {
    const auto b = bundle();
    const auto i = intent();
    MarketDepthView d;
    d.asset_index = 7;
    d.usable_for_depth = true;
    const auto p = policy();

    const auto result = BundleSizeOptimizer{}.optimize({
        .intent = &i,
        .bundle = &b,
        .depth_views = std::span<const MarketDepthView>(&d, 1),
        .policy = &p
    });

    expect_false(result.ok, "ok");
    expect_equal(result.reject_reason, OrderDecisionType::RejectNoDepth, "reason");
}

void Optimizer_TieBreaksByEdgeBpsThenCost() {
    const auto b = bundle();
    const auto i = intent();
    const auto d = depth({
        PriceLevel{.price_tick = 500'000, .price = 0.5, .size = 1.0},
        PriceLevel{.price_tick = 1'000'000, .price = 1.0, .size = 1.0}
    });
    const auto p = policy();

    const auto result = BundleSizeOptimizer{}.optimize({
        .intent = &i,
        .bundle = &b,
        .depth_views = std::span<const MarketDepthView>(&d, 1),
        .policy = &p
    });

    expect_true(result.ok, "ok");
    expect_equal(result.best_bundle_qty, 1LL, "qty");
    expect_equal(result.total_edge_tick, 500'000LL, "edge");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"Optimizer_PicksMaxTotalEdge", &Optimizer_PicksMaxTotalEdge},
        {"Optimizer_RejectsLowUnitEdge", &Optimizer_RejectsLowUnitEdge},
        {"Optimizer_RejectsLowTotalEdge", &Optimizer_RejectsLowTotalEdge},
        {"Optimizer_RejectsInsufficientDepth", &Optimizer_RejectsInsufficientDepth},
        {
            "Optimizer_TieBreaksByEdgeBpsThenCost",
            &Optimizer_TieBreaksByEdgeBpsThenCost
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
