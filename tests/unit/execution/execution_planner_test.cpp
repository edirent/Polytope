#include "engine/execution/plan/ExecutionPlanner.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::ApprovedIntentEnvelope;
using trading_engine::execution::ExecutionApproval;
using trading_engine::execution::ExecutionConfig;
using trading_engine::execution::ExecutionPlanner;
using trading_engine::execution::OrderSide;
using trading_engine::oracle::Side;
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

ApprovedIntentEnvelope valid_envelope() {
    ApprovedIntentEnvelope envelope;

    auto& intent = envelope.source_intent;
    intent.intent_id = 101;
    intent.bundle_id = 202;
    intent.status = IntentStatus::PaperOpportunity;
    intent.estimated_cost_tick = 900;
    intent.total_edge_tick = 100;
    intent.slippage_buffer_tick = 3;
    intent.created_ts_ns = 1000;
    intent.expires_at_ns = 3000;
    intent.idempotency_key = "intent-101:bundle-202";
    intent.leg_count = 2;

    intent.legs[0].market_id = "market-a";
    intent.legs[0].asset_id = "asset-a";
    intent.legs[0].side = Side::Buy;
    intent.legs[0].quantity_lots = 1;
    intent.legs[0].estimated_vwap_tick = 41;
    intent.legs[0].worst_price_tick = 45;

    intent.legs[1].market_id = "market-b";
    intent.legs[1].asset_id = "asset-b";
    intent.legs[1].side = Side::Buy;
    intent.legs[1].quantity_lots = 2;
    intent.legs[1].estimated_vwap_tick = 50;
    intent.legs[1].worst_price_tick = 55;

    auto& approval = envelope.approval;
    approval.decision_id = 303;
    approval.reservation_id = 404;
    approval.bundle_id = intent.bundle_id;
    approval.approved_bundle_qty = 10;
    approval.idempotency_key = intent.idempotency_key;

    return envelope;
}

ExecutionConfig test_config() {
    ExecutionConfig config;
    config.max_child_orders_per_plan = 16;
    return config;
}

void ExecutionPlanner_BuildsPlanFromApprovedIntent() {
    const auto envelope = valid_envelope();
    const ExecutionPlanner planner;

    const auto result = planner.build_plan(envelope, 1500, test_config());

    expect_true(result.ok, "plan built");
    expect_equal(result.plan.source_intent_id, 101ULL, "source_intent_id");
    expect_equal(result.plan.approved_intent_id, 303ULL, "approved_intent_id");
    expect_equal(result.plan.reservation_id, 404ULL, "reservation_id");
    expect_equal(result.plan.bundle_id, 202ULL, "bundle_id");
    expect_equal(result.plan.order_count, static_cast<std::uint16_t>(2), "order_count");
    expect_equal(result.plan.created_ts_ns, 1500ULL, "created_ts_ns");
    expect_equal(result.plan.expire_after_ns, 3000ULL, "expire_after_ns");
    expect_equal(
        result.plan.idempotency_key,
        std::string{"intent-101:bundle-202"},
        "idempotency_key"
    );

    const auto& first = result.plan.orders[0];
    expect_equal(first.plan_id, result.plan.plan_id, "first.plan_id");
    expect_equal(first.market_id, std::string{"market-a"}, "first.market_id");
    expect_equal(first.asset_id, std::string{"asset-a"}, "first.asset_id");
    expect_equal(first.side, OrderSide::Buy, "first.side");
    expect_equal(first.quantity_lots, 10LL, "first.quantity_lots");
    expect_equal(first.limit_price_tick, 45LL, "first.limit_price_tick");
    expect_equal(first.estimated_vwap_tick, 41LL, "first.estimated_vwap_tick");
    expect_equal(
        first.worst_allowed_price_tick,
        45LL,
        "first.worst_allowed_price_tick"
    );

    const auto& second = result.plan.orders[1];
    expect_equal(second.quantity_lots, 20LL, "second.quantity_lots");
    expect_equal(second.limit_price_tick, 55LL, "second.limit_price_tick");
}

void ExecutionPlanner_RejectsMissingReservation() {
    auto envelope = valid_envelope();
    envelope.approval.reservation_id = 0;

    const ExecutionPlanner planner;
    const auto result = planner.build_plan(envelope, 1500, test_config());

    expect_true(!result.ok, "missing reservation rejected");
}

void ExecutionPlanner_RejectsExpiredIntent() {
    auto envelope = valid_envelope();
    envelope.source_intent.expires_at_ns = 1500;

    const ExecutionPlanner planner;
    const auto result = planner.build_plan(envelope, 1500, test_config());

    expect_true(!result.ok, "expired intent rejected");
}

void ExecutionPlanner_RejectsMismatchedIdempotencyKey() {
    auto envelope = valid_envelope();
    envelope.approval.idempotency_key = "different-key";

    const ExecutionPlanner planner;
    const auto result = planner.build_plan(envelope, 1500, test_config());

    expect_true(!result.ok, "mismatched idempotency key rejected");
}

void ExecutionPlanner_RejectsSellLegInV0() {
    auto envelope = valid_envelope();
    envelope.source_intent.legs[0].side = Side::Sell;

    const ExecutionPlanner planner;
    const auto result = planner.build_plan(envelope, 1500, test_config());

    expect_true(!result.ok, "sell leg rejected");
    expect_equal(result.error, std::string{"UnsupportedSide"}, "error");
}

void ExecutionPlanner_GeneratesDeterministicClientOrderIds() {
    const auto envelope = valid_envelope();
    const ExecutionPlanner planner;

    const auto first = planner.build_plan(envelope, 1500, test_config());
    const auto second = planner.build_plan(envelope, 1500, test_config());

    expect_true(first.ok, "first plan built");
    expect_true(second.ok, "second plan built");
    expect_equal(first.plan.plan_id, second.plan.plan_id, "plan_id");
    expect_equal(
        first.plan.orders[0].client_order_id,
        second.plan.orders[0].client_order_id,
        "first client_order_id"
    );
    expect_equal(
        first.plan.orders[1].client_order_id,
        second.plan.orders[1].client_order_id,
        "second client_order_id"
    );
    expect_true(
        first.plan.orders[0].client_order_id !=
            first.plan.orders[1].client_order_id,
        "per-order client_order_id differs"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "ExecutionPlanner_BuildsPlanFromApprovedIntent",
            &ExecutionPlanner_BuildsPlanFromApprovedIntent
        },
        {
            "ExecutionPlanner_RejectsMissingReservation",
            &ExecutionPlanner_RejectsMissingReservation
        },
        {
            "ExecutionPlanner_RejectsExpiredIntent",
            &ExecutionPlanner_RejectsExpiredIntent
        },
        {
            "ExecutionPlanner_RejectsMismatchedIdempotencyKey",
            &ExecutionPlanner_RejectsMismatchedIdempotencyKey
        },
        {
            "ExecutionPlanner_RejectsSellLegInV0",
            &ExecutionPlanner_RejectsSellLegInV0
        },
        {
            "ExecutionPlanner_GeneratesDeterministicClientOrderIds",
            &ExecutionPlanner_GeneratesDeterministicClientOrderIds
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
