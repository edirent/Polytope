#include "engine/order_decision/sizing/CostCurveBuilder.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::order_decision::CostCurveBuildInput;
using trading_engine::order_decision::CostCurveBuilder;
using trading_engine::order_decision::OrderDecisionType;
using trading_engine::oracle::BundleLeg;
using trading_engine::oracle::Side;
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

BundleLeg buy_leg() {
    return {
        .market_id = "m1",
        .asset_id = "asset_yes",
        .side = Side::Buy,
        .quantity_lots = 1,
        .max_price_tick = 1'000'000
    };
}

MarketDepthView depth_with_asks(std::initializer_list<PriceLevel> asks) {
    MarketDepthView depth;
    depth.asset_index = 7;
    depth.usable_for_depth = true;
    for (const auto& ask : asks) {
        depth.asks[depth.ask_count++] = ask;
    }
    return depth;
}

void CostCurveBuilder_BuildsAskCurve() {
    auto leg = buy_leg();
    auto depth = depth_with_asks({
        PriceLevel{.price_tick = 100'000, .price = 0.1, .size = 5.0},
        PriceLevel{.price_tick = 200'000, .price = 0.2, .size = 3.0}
    });

    const auto result = CostCurveBuilder{}.build_buy_curve({
        .leg = &leg,
        .depth = &depth,
        .asset_index = depth.asset_index
    });

    expect_true(result.ok, "ok");
    expect_equal(result.curve.level_count, static_cast<std::uint16_t>(2), "levels");
    expect_equal(result.curve.total_qty_lots, 8LL, "total_qty");
}

void CostCurveBuilder_RejectsNoAskDepth() {
    auto leg = buy_leg();
    MarketDepthView depth;
    depth.asset_index = 7;

    const auto result = CostCurveBuilder{}.build_buy_curve({
        .leg = &leg,
        .depth = &depth,
        .asset_index = depth.asset_index
    });

    expect_false(result.ok, "ok");
    expect_equal(result.reject_reason, OrderDecisionType::RejectNoDepth, "reason");
}

void CostCurveBuilder_PreservesCumulativeCost() {
    auto leg = buy_leg();
    auto depth = depth_with_asks({
        PriceLevel{.price_tick = 100'000, .price = 0.1, .size = 5.0},
        PriceLevel{.price_tick = 200'000, .price = 0.2, .size = 3.0}
    });

    const auto result = CostCurveBuilder{}.build_buy_curve({
        .leg = &leg,
        .depth = &depth,
        .asset_index = depth.asset_index
    });

    expect_true(result.ok, "ok");
    expect_equal(result.curve.levels[0].cumulative_cost_tick, 500'000LL, "l0");
    expect_equal(result.curve.levels[1].cumulative_cost_tick, 1'100'000LL, "l1");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"CostCurveBuilder_BuildsAskCurve", &CostCurveBuilder_BuildsAskCurve},
        {"CostCurveBuilder_RejectsNoAskDepth", &CostCurveBuilder_RejectsNoAskDepth},
        {
            "CostCurveBuilder_PreservesCumulativeCost",
            &CostCurveBuilder_PreservesCumulativeCost
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
