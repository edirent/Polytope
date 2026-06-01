#include "engine/order_decision/core/OrderDecisionEngine.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "state/book/DepthPrefix.h"
#include "state/view/MarketDepthView.h"

#include <exception>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

namespace decision = trading_engine::order_decision;
namespace oracle = trading_engine::oracle;
namespace risk = trading_engine::risk;
namespace signal = trading_engine::signal;
namespace state = trading_engine::state;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
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

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

oracle::CandidateBundle bundle() {
    oracle::CandidateBundle out;
    out.bundle_id = 10;
    out.guaranteed_payout_tick = 1'000'000;
    out.leg_count = 1;
    out.legs[0] = oracle::BundleLeg{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .side = oracle::Side::Buy,
        .quantity_lots = 1,
        .max_price_tick = 1'000'000
    };
    return out;
}

signal::OpportunityIntent intent() {
    signal::OpportunityIntent out;
    out.intent_id = 42;
    out.bundle_id = 10;
    out.status = signal::IntentStatus::PaperOpportunity;
    out.guaranteed_payout_tick = 1'000'000;
    out.leg_count = 1;
    out.legs[0].market_id = "m1";
    out.legs[0].asset_id = "asset_yes";
    out.legs[0].asset_index = 7;
    out.legs[0].side = oracle::Side::Buy;
    out.legs[0].quantity_lots = 1;
    out.snapshot_version_hash = 123;
    out.bundle_hash = 456;
    return out;
}

state::MarketDepthView depth() {
    state::MarketDepthView view;
    view.asset_index = 7;
    view.usable_for_depth = true;
    view.ask_count = 3;
    view.asks[0] = state::PriceLevel{
        .price_tick = 100'000,
        .price = 0.10,
        .size = 10.0
    };
    view.asks[1] = state::PriceLevel{
        .price_tick = 200'000,
        .price = 0.20,
        .size = 10.0
    };
    view.asks[2] = state::PriceLevel{
        .price_tick = 300'000,
        .price = 0.30,
        .size = 10.0
    };
    state::build_depth_prefix(
        view.bids,
        view.bid_count,
        view.asks,
        view.ask_count,
        &view.prefix
    );
    return view;
}

risk::RiskPolicySnapshot policy() {
    risk::RiskPolicySnapshot out;
    out.max_total_cost_tick = 100'000'000;
    out.min_depth_margin_bps = 10'000;
    return risk::with_computed_policy_hash(out);
}

decision::OrderDecisionResult decide_with_prefix(bool enabled) {
    auto config = decision::OrderDecisionConfig{};
    config.use_prefix_vwap = enabled;
    config.debug_compare_prefix_vs_linear = enabled;
    const auto b = bundle();
    const auto i = intent();
    const auto d = depth();
    const auto p = policy();
    return decision::OrderDecisionEngine{config}.decide(
        i,
        b,
        std::span<const state::MarketDepthView>(&d, 1),
        p,
        1
    );
}

void OrderDecision_PrefixMatchesLinearOnSmallFixture() {
    const auto linear = decide_with_prefix(false);
    const auto prefix = decide_with_prefix(true);

    expect_true(linear.ok, "linear ok");
    expect_true(prefix.ok, "prefix ok");
    expect_equal(
        prefix.decision.chosen_bundle_qty,
        linear.decision.chosen_bundle_qty,
        "qty"
    );
    expect_equal(
        prefix.decision.estimated_total_cost_tick,
        linear.decision.estimated_total_cost_tick,
        "cost"
    );
    expect_equal(
        prefix.decision.total_edge_tick,
        linear.decision.total_edge_tick,
        "edge"
    );
    expect_equal(
        prefix.decision.decision_hash,
        linear.decision.decision_hash,
        "hash"
    );
    expect_equal(
        prefix.eval_stats.prefix_linear_mismatch,
        static_cast<std::uint32_t>(0),
        "mismatch"
    );
    expect_true(
        prefix.eval_stats.incremental_eval_steps > 0,
        "prefix config uses incremental eval"
    );
    expect_true(
        linear.eval_stats.incremental_eval_steps > 0,
        "linear config uses incremental eval"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "OrderDecision_PrefixMatchesLinearOnSmallFixture",
            &OrderDecision_PrefixMatchesLinearOnSmallFixture
        },
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
