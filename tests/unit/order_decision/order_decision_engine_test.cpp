#include "engine/order_decision/core/OrderDecisionEngine.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::order_decision::OrderDecisionConfig;
using trading_engine::order_decision::OrderDecisionEngine;
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

CandidateBundle bundle(Side side = Side::Buy) {
    CandidateBundle out;
    out.bundle_id = 10;
    out.guaranteed_payout_tick = 1'000'000;
    out.leg_count = 1;
    out.legs[0] = BundleLeg{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .side = side,
        .quantity_lots = 1,
        .max_price_tick = 1'000'000
    };
    return out;
}

OpportunityIntent intent(std::uint64_t expires_at_ns = 1'000) {
    OpportunityIntent out;
    out.intent_id = 42;
    out.bundle_id = 10;
    out.status = IntentStatus::PaperOpportunity;
    out.guaranteed_payout_tick = 1'000'000;
    out.snapshot_version_hash = 11;
    out.oracle_artifact_hash = 22;
    out.bundle_hash = 33;
    out.expires_at_ns = expires_at_ns;
    out.leg_count = 1;
    out.legs[0].market_id = "m1";
    out.legs[0].asset_id = "asset_yes";
    out.legs[0].asset_index = 7;
    out.legs[0].side = Side::Buy;
    out.legs[0].quantity_lots = 1;
    return out;
}

MarketDepthView depth() {
    MarketDepthView out;
    out.asset_index = 7;
    out.usable_for_depth = true;
    out.snapshot_version_hash = 11;
    out.asks[out.ask_count++] =
        PriceLevel{.price_tick = 100'000, .price = 0.1, .size = 10.0};
    return out;
}

RiskPolicySnapshot policy() {
    RiskPolicySnapshot out;
    out.max_total_cost_tick = 100'000'000;
    out.min_depth_margin_bps = 10'000;
    return with_computed_policy_hash(out);
}

void OrderDecisionEngine_ProducesDecisionForBuyOnlyBundle() {
    const auto b = bundle();
    const auto i = intent();
    const auto d = depth();
    const auto p = policy();
    OrderDecisionConfig config;
    config.price_protection_buffer_tick = 1'000;

    const auto result = OrderDecisionEngine{config}.decide(
        i,
        b,
        std::span<const MarketDepthView>(&d, 1),
        p,
        100
    );

    expect_true(result.ok, "ok");
    expect_equal(
        result.decision.type,
        OrderDecisionType::PaperOrderDecision,
        "type"
    );
    expect_equal(result.decision.chosen_bundle_qty, 10LL, "qty");
    expect_equal(result.decision.legs[0].limit_price_tick, 101'000LL, "limit");
}

void OrderDecisionEngine_RejectsSellInV1() {
    auto b = bundle(Side::Sell);
    auto i = intent();
    i.legs[0].side = Side::Sell;
    const auto d = depth();
    const auto p = policy();

    const auto result = OrderDecisionEngine{}.decide(
        i,
        b,
        std::span<const MarketDepthView>(&d, 1),
        p,
        100
    );

    expect_false(result.ok, "ok");
    expect_equal(
        result.reject_reason,
        OrderDecisionType::RejectUnsupportedSide,
        "reason"
    );
}

void OrderDecisionEngine_RejectsExpiredIntent() {
    const auto b = bundle();
    const auto i = intent(99);
    const auto d = depth();
    const auto p = policy();

    const auto result = OrderDecisionEngine{}.decide(
        i,
        b,
        std::span<const MarketDepthView>(&d, 1),
        p,
        100
    );

    expect_false(result.ok, "ok");
    expect_equal(
        result.reject_reason,
        OrderDecisionType::RejectExpiredIntent,
        "reason"
    );
}

void OrderDecisionEngine_DeterministicDecisionHash() {
    const auto b = bundle();
    const auto i = intent();
    const auto d = depth();
    const auto p = policy();

    const auto a = OrderDecisionEngine{}.decide(
        i,
        b,
        std::span<const MarketDepthView>(&d, 1),
        p,
        100
    );
    const auto b_result = OrderDecisionEngine{}.decide(
        i,
        b,
        std::span<const MarketDepthView>(&d, 1),
        p,
        100
    );

    expect_true(a.ok, "a ok");
    expect_true(b_result.ok, "b ok");
    expect_equal(
        a.decision.decision_hash,
        b_result.decision.decision_hash,
        "hash"
    );
}

void OrderDecisionEngine_FastFixedShapeReturnsOrderDecisionResult() {
    const auto b = bundle();
    const auto i = intent();
    const auto d = depth();
    const auto p = policy();
    OrderDecisionConfig config;
    config.impl_mode =
        trading_engine::order_decision::OrderDecisionImplMode::FastFixedShape;
    config.fast_fixed_shape_fallback_to_generic = false;

    const auto result = OrderDecisionEngine{config}.decide(
        i,
        b,
        std::span<const MarketDepthView>(&d, 1),
        p,
        100
    );

    expect_true(result.ok, "ok");
    expect_equal(
        result.decision.type,
        OrderDecisionType::PaperOrderDecision,
        "type"
    );
    expect_equal(result.decision.chosen_bundle_qty, 10LL, "qty");
    expect_equal(result.decision.source_intent_id, i.intent_id, "intent_id");
}

void OrderDecisionEngine_FastFixedShapeRejectsSellLeg() {
    auto b = bundle(Side::Sell);
    auto i = intent();
    i.legs[0].side = Side::Sell;
    const auto d = depth();
    const auto p = policy();
    OrderDecisionConfig config;
    config.impl_mode =
        trading_engine::order_decision::OrderDecisionImplMode::FastFixedShape;
    config.fast_fixed_shape_fallback_to_generic = false;

    const auto result = OrderDecisionEngine{config}.decide(
        i,
        b,
        std::span<const MarketDepthView>(&d, 1),
        p,
        100
    );

    expect_false(result.ok, "ok");
    expect_equal(
        result.reject_reason,
        OrderDecisionType::RejectUnsupportedSide,
        "reason"
    );
}

void OrderDecisionEngine_FastFixedShapeMatchesGenericPositive() {
    const auto b = bundle();
    const auto i = intent();
    const auto d = depth();
    const auto p = policy();
    OrderDecisionConfig fast_config;
    fast_config.impl_mode =
        trading_engine::order_decision::OrderDecisionImplMode::FastFixedShape;
    fast_config.fast_fixed_shape_fallback_to_generic = false;

    const auto generic = OrderDecisionEngine{}.decide(
        i,
        b,
        std::span<const MarketDepthView>(&d, 1),
        p,
        100
    );
    const auto fast = OrderDecisionEngine{fast_config}.decide(
        i,
        b,
        std::span<const MarketDepthView>(&d, 1),
        p,
        100
    );

    expect_true(generic.ok, "generic ok");
    expect_true(fast.ok, "fast ok");
    expect_equal(
        fast.decision.chosen_bundle_qty,
        generic.decision.chosen_bundle_qty,
        "qty"
    );
    expect_equal(
        fast.decision.estimated_total_cost_tick,
        generic.decision.estimated_total_cost_tick,
        "cost"
    );
    expect_equal(
        fast.decision.total_edge_tick,
        generic.decision.total_edge_tick,
        "edge"
    );
}

void OrderDecisionEngine_GenericMemoCacheExactHit() {
    const auto b = bundle();
    const auto i = intent();
    const auto d = depth();
    const auto p = policy();
    OrderDecisionEngine engine;

    const auto first = engine.decide(
        i,
        b,
        std::span<const MarketDepthView>(&d, 1),
        p,
        100
    );
    const auto second = engine.decide(
        i,
        b,
        std::span<const MarketDepthView>(&d, 1),
        p,
        100
    );

    expect_true(first.ok, "first ok");
    expect_true(second.ok, "second ok");
    expect_equal(
        second.eval_stats.decision_memo_cache_hits,
        1U,
        "memo hit"
    );
    expect_equal(
        first.decision.decision_hash,
        second.decision.decision_hash,
        "memo decision hash"
    );
}

void OrderDecisionEngine_GenericCandidateCacheAndIncrementalEval() {
    const auto b = bundle();
    const auto i = intent();
    const auto d = depth();
    const auto p = policy();
    OrderDecisionEngine engine;

    const auto first = engine.decide(
        i,
        b,
        std::span<const MarketDepthView>(&d, 1),
        p,
        100
    );
    const auto second = engine.decide(
        i,
        b,
        std::span<const MarketDepthView>(&d, 1),
        p,
        101
    );

    expect_true(first.ok, "first ok");
    expect_true(second.ok, "second ok");
    expect_equal(
        second.eval_stats.candidate_set_cache_hits,
        1U,
        "candidate set hit"
    );
    expect_true(
        second.eval_stats.incremental_eval_steps > 0,
        "incremental eval used"
    );
    expect_equal(
        second.eval_stats.prefix_cost_calls,
        0U,
        "incremental eval avoids prefix cost calls"
    );
}

void OrderDecisionEngine_FastFixedShapeSpecCacheHit() {
    const auto b = bundle();
    const auto i = intent();
    const auto d = depth();
    const auto p = policy();
    OrderDecisionConfig config;
    config.impl_mode =
        trading_engine::order_decision::OrderDecisionImplMode::FastFixedShape;
    config.fast_fixed_shape_fallback_to_generic = false;
    OrderDecisionEngine engine{config};

    const auto first = engine.decide(
        i,
        b,
        std::span<const MarketDepthView>(&d, 1),
        p,
        100
    );
    const auto second = engine.decide(
        i,
        b,
        std::span<const MarketDepthView>(&d, 1),
        p,
        101
    );

    expect_true(first.ok, "first ok");
    expect_true(second.ok, "second ok");
    expect_equal(
        second.eval_stats.fixed_shape_spec_cache_hits,
        1U,
        "spec hit"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "OrderDecisionEngine_ProducesDecisionForBuyOnlyBundle",
            &OrderDecisionEngine_ProducesDecisionForBuyOnlyBundle
        },
        {"OrderDecisionEngine_RejectsSellInV1", &OrderDecisionEngine_RejectsSellInV1},
        {
            "OrderDecisionEngine_RejectsExpiredIntent",
            &OrderDecisionEngine_RejectsExpiredIntent
        },
        {
            "OrderDecisionEngine_DeterministicDecisionHash",
            &OrderDecisionEngine_DeterministicDecisionHash
        },
        {
            "OrderDecisionEngine_FastFixedShapeReturnsOrderDecisionResult",
            &OrderDecisionEngine_FastFixedShapeReturnsOrderDecisionResult
        },
        {
            "OrderDecisionEngine_FastFixedShapeRejectsSellLeg",
            &OrderDecisionEngine_FastFixedShapeRejectsSellLeg
        },
        {
            "OrderDecisionEngine_FastFixedShapeMatchesGenericPositive",
            &OrderDecisionEngine_FastFixedShapeMatchesGenericPositive
        },
        {
            "OrderDecisionEngine_GenericMemoCacheExactHit",
            &OrderDecisionEngine_GenericMemoCacheExactHit
        },
        {
            "OrderDecisionEngine_GenericCandidateCacheAndIncrementalEval",
            &OrderDecisionEngine_GenericCandidateCacheAndIncrementalEval
        },
        {
            "OrderDecisionEngine_FastFixedShapeSpecCacheHit",
            &OrderDecisionEngine_FastFixedShapeSpecCacheHit
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
