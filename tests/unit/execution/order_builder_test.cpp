#include "engine/execution/plan/OrderBuilder.h"
#include "engine/risk/public/RiskDecision.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::ExecutionContext;
using trading_engine::execution::OrderBuilder;
using trading_engine::execution::OrderSide;
using trading_engine::oracle::Side;
using trading_engine::risk::ApprovedIntent;
using trading_engine::risk::make_approved_decision;
using trading_engine::signal::IntentStatus;

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

ApprovedIntent valid_approved_intent(std::string reservation_id = "404") {
    ApprovedIntent approved;
    approved.decision = make_approved_decision(1, 2);
    approved.reservation_id = std::move(reservation_id);
    approved.approved_at_ns = 900;
    approved.expires_at_ns = 2000;

    auto& intent = approved.intent;
    intent.intent_id = 101;
    intent.bundle_id = 202;
    intent.status = IntentStatus::PaperOpportunity;
    intent.estimated_cost_tick = 800;
    intent.total_edge_tick = 120;
    intent.slippage_buffer_tick = 7;
    intent.idempotency_key = "intent-101:bundle-202";
    intent.leg_count = 1;

    auto& leg = intent.legs[0];
    leg.market_id = "market-a";
    leg.asset_id = "asset-a";
    leg.side = Side::Buy;
    leg.quantity_lots = 3;
    leg.estimated_vwap_tick = 42;
    leg.worst_price_tick = 45;
    leg.estimated_cost_tick = 126;
    return approved;
}

void OrderBuilder_BuildsPlanFromApprovedIntent() {
    const OrderBuilder builder;
    ExecutionContext context;
    context.now_ns = 1000;

    const auto plan = builder.build(valid_approved_intent(), context);

    expect_equal(plan.plan_id, 101ULL, "plan_id");
    expect_equal(plan.source_intent_id, 101ULL, "source_intent_id");
    expect_equal(plan.approved_intent_id, 101ULL, "approved_intent_id");
    expect_equal(plan.reservation_id, 404ULL, "reservation_id");
    expect_equal(plan.bundle_id, 202ULL, "bundle_id");
    expect_equal(plan.order_count, static_cast<std::uint16_t>(1), "order_count");
    expect_equal(plan.max_total_cost_tick, 800LL, "max_total_cost_tick");
    expect_equal(plan.min_expected_edge_tick, 120LL, "min_expected_edge_tick");
    expect_equal(plan.max_slippage_tick, 7LL, "max_slippage_tick");
    expect_equal(plan.created_ts_ns, 1000ULL, "created_ts_ns");
    expect_equal(plan.expire_after_ns, 2000ULL, "expire_after_ns");

    const auto& order = plan.orders[0];
    expect_equal(order.order_id, 1ULL, "order_id");
    expect_equal(order.plan_id, plan.plan_id, "order.plan_id");
    expect_equal(order.client_order_id, std::string{"intent-101:bundle-202-1"}, "client_order_id");
    expect_equal(order.market_id, std::string{"market-a"}, "market_id");
    expect_equal(order.asset_id, std::string{"asset-a"}, "asset_id");
    expect_equal(order.side, OrderSide::Buy, "side");
    expect_equal(order.quantity_lots, 3LL, "quantity_lots");
    expect_equal(order.limit_price_tick, 45LL, "limit_price_tick");
    expect_equal(order.estimated_vwap_tick, 42LL, "estimated_vwap_tick");
    expect_equal(order.worst_allowed_price_tick, 45LL, "worst_allowed_price_tick");
}

void OrderBuilder_HashesNonNumericReservationIdDeterministically() {
    const OrderBuilder builder;
    ExecutionContext context;
    context.now_ns = 1000;

    const auto first = builder.build(
        valid_approved_intent("reservation-fixture"),
        context
    );
    const auto second = builder.build(
        valid_approved_intent("reservation-fixture"),
        context
    );

    expect_true(first.reservation_id != 0, "hashed reservation nonzero");
    expect_equal(
        first.reservation_id,
        second.reservation_id,
        "reservation_id deterministic"
    );
}

void OrderBuilder_PreservesLegPricingFields() {
    const OrderBuilder builder;
    ExecutionContext context;
    context.now_ns = 1000;

    const auto plan = builder.build(valid_approved_intent(), context);

    expect_equal(plan.orders[0].limit_price_tick, 45LL, "limit_price_tick");
    expect_equal(plan.orders[0].estimated_vwap_tick, 42LL, "estimated_vwap_tick");
    expect_equal(
        plan.orders[0].worst_allowed_price_tick,
        45LL,
        "worst_allowed_price_tick"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "OrderBuilder_BuildsPlanFromApprovedIntent",
            &OrderBuilder_BuildsPlanFromApprovedIntent
        },
        {
            "OrderBuilder_HashesNonNumericReservationIdDeterministically",
            &OrderBuilder_HashesNonNumericReservationIdDeterministically
        },
        {
            "OrderBuilder_PreservesLegPricingFields",
            &OrderBuilder_PreservesLegPricingFields
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
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }
    return run_test(argv[1]);
}
