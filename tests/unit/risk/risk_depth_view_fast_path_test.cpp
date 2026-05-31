#include "engine/risk/reprice/CostRevalidator.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/view/MarketDepthView.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::risk::CostRevalidator;
using trading_engine::risk::RiskPolicySnapshot;
using trading_engine::risk::RiskVWAPMode;
using trading_engine::signal::IntentStatus;
using trading_engine::signal::OpportunityIntent;
using trading_engine::signal::Side;
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
    out.bundle_qty = 2;
    out.original_bundle_qty = 2;
    out.estimated_cost_tick = 5'000'000;
    out.snapshot_version_hash = 999;
    out.expires_at_ns = 20'000;
    out.leg_count = 1;
    out.legs[0].market_id = "m1";
    out.legs[0].asset_id = "asset-yes";
    out.legs[0].asset_index = 5;
    out.legs[0].side = Side::Buy;
    out.legs[0].quantity_lots = 5;
    out.legs[0].requested_qty_lots = 10;
    out.legs[0].executable_qty_lots = 10;
    out.legs[0].depth_margin_bps = 10'000;
    out.legs[0].enough_depth = true;
    return out;
}

MarketDepthView depth_view() {
    MarketDepthView view;
    view.asset_index = 5;
    view.book_version = 10;
    view.snapshot_version_hash = 999;
    view.last_ws_recv_ns = 10'000;
    view.usable_for_depth = true;
    view.ask_count = 1;
    view.asks[0] = PriceLevel{.price_tick = 500'000, .price = 0.50, .size = 10.0};
    return view;
}

void RiskUsesDepthViewForFastRevalidation() {
    RiskPolicySnapshot policy;
    policy.max_book_age_ns = 10'000;
    const auto input_intent = intent();
    const auto depth = depth_view();

    const auto result = CostRevalidator{}.revalidate(
        input_intent,
        &depth,
        1,
        policy,
        10'000,
        input_intent.snapshot_version_hash,
        nullptr
    );

    expect_true(result.ok, "ok");
    expect_equal(
        result.vwap_mode,
        RiskVWAPMode::ReuseSignalSnapshot,
        "vwap mode"
    );
    expect_equal(result.risk_total_cost_tick, 5'000'000LL, "cost");
    expect_equal(result.risk_bundle_qty, 2LL, "bundle qty");
}

void RiskDepthViewFallbackRepricesOnHashMismatch() {
    RiskPolicySnapshot policy;
    policy.max_book_age_ns = 10'000;
    auto input_intent = intent();
    input_intent.snapshot_version_hash = 123;
    const auto depth = depth_view();

    const auto result = CostRevalidator{}.revalidate(
        input_intent,
        &depth,
        1,
        policy,
        10'000,
        depth.snapshot_version_hash,
        nullptr
    );

    expect_true(result.ok, "ok");
    expect_equal(
        result.vwap_mode,
        RiskVWAPMode::RecomputedFromSnapshot,
        "vwap mode"
    );
    expect_equal(result.risk_total_cost_tick, 5'000'000LL, "cost");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"RiskUsesDepthViewForFastRevalidation",
         &RiskUsesDepthViewForFastRevalidation},
        {"RiskDepthViewFallbackRepricesOnHashMismatch",
         &RiskDepthViewFallbackRepricesOnHashMismatch},
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
