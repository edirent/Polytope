#include "engine/risk/reprice/VWAPRevalidator.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/view/MarketDepthView.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::risk::VWAPRevalidator;
using trading_engine::signal::IntentStatus;
using trading_engine::signal::OpportunityIntent;
using trading_engine::signal::Side;
using trading_engine::state::MarketDepthView;
using trading_engine::state::PriceLevel;
using trading_engine::state::build_depth_prefix;

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

OpportunityIntent intent() {
    OpportunityIntent out;
    out.intent_id = 1;
    out.bundle_id = 2;
    out.status = IntentStatus::PaperOpportunity;
    out.bundle_qty = 1;
    out.leg_count = 1;
    out.legs[0].market_id = "m1";
    out.legs[0].asset_id = "asset-yes";
    out.legs[0].asset_index = 5;
    out.legs[0].side = Side::Buy;
    out.legs[0].quantity_lots = 15;
    return out;
}

MarketDepthView depth_view(bool with_prefix) {
    MarketDepthView view;
    view.asset_index = 5;
    view.snapshot_version_hash = 99;
    view.usable_for_depth = true;
    view.ask_count = 2;
    view.asks[0] = PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 10.0};
    view.asks[1] = PriceLevel{.price_tick = 550'000, .price = 0.55, .size = 10.0};
    if (with_prefix) {
        build_depth_prefix(
            view.bids,
            view.bid_count,
            view.asks,
            view.ask_count,
            &view.prefix
        );
    }
    return view;
}

void RiskVWAPRevalidator_PrefixMultiLevel() {
    const auto input_intent = intent();
    const auto depth = depth_view(true);

    const auto result = VWAPRevalidator{}.reprice(input_intent, &depth, 1);

    expect_true(result.ok, "ok");
    expect_equal(result.risk_total_cost_tick, 7'750'000LL, "cost");
    expect_equal(result.risk_bundle_qty, 1LL, "bundle qty");
    expect_equal(result.legs[0].executable_qty_lots, 20LL, "depth");
}

void RiskVWAPRevalidator_PrefixEqualsLinearDepth() {
    const auto input_intent = intent();
    const auto prefix_depth = depth_view(true);
    const auto linear_depth = depth_view(false);

    const auto prefix_result =
        VWAPRevalidator{}.reprice(input_intent, &prefix_depth, 1);
    const auto linear_result =
        VWAPRevalidator{}.reprice(input_intent, &linear_depth, 1);

    expect_true(prefix_result.ok, "prefix ok");
    expect_true(linear_result.ok, "linear ok");
    expect_equal(
        prefix_result.risk_total_cost_tick,
        linear_result.risk_total_cost_tick,
        "cost"
    );
    expect_equal(
        prefix_result.risk_bundle_qty,
        linear_result.risk_bundle_qty,
        "bundle qty"
    );
    expect_equal(
        prefix_result.legs[0].depth_margin_bps,
        linear_result.legs[0].depth_margin_bps,
        "depth margin"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"RiskVWAPRevalidator_PrefixMultiLevel",
         &RiskVWAPRevalidator_PrefixMultiLevel},
        {"RiskVWAPRevalidator_PrefixEqualsLinearDepth",
         &RiskVWAPRevalidator_PrefixEqualsLinearDepth},
    };
    return test_map;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }

    const auto it = tests().find(argv[1]);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << argv[1] << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << argv[1] << " failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
