#include "engine/execution/plan/PlanValidator.h"
#include "engine/execution/public/OrderPlan.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::OrderPlan;
using trading_engine::execution::PlanStatus;
using trading_engine::execution::PlanValidator;
using trading_engine::execution::kMaxChildOrdersPerPlan;

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

OrderPlan valid_order_plan() {
    OrderPlan plan;
    plan.plan_id = 1;
    plan.source_intent_id = 2;
    plan.approved_intent_id = 3;
    plan.reservation_id = 4;
    plan.bundle_id = 5;
    plan.order_count = 1;
    plan.max_total_cost_tick = 100;
    plan.min_expected_edge_tick = 10;
    plan.max_slippage_tick = 2;
    plan.created_ts_ns = 1000;
    plan.expire_after_ns = 2000;
    plan.idempotency_key = "intent-2:bundle-5";

    auto& order = plan.orders[0];
    order.order_id = 11;
    order.plan_id = plan.plan_id;
    order.client_order_id = "intent-2:bundle-5-1";
    order.market_id = "market-1";
    order.asset_id = "asset-1";
    order.quantity_lots = 25;
    order.limit_price_tick = 120;
    order.estimated_vwap_tick = 115;
    order.worst_allowed_price_tick = 120;
    return plan;
}

void OrderPlan_DefaultsCreated() {
    const OrderPlan plan;

    expect_equal(plan.plan_id, 0ULL, "plan_id");
    expect_equal(plan.source_intent_id, 0ULL, "source_intent_id");
    expect_equal(plan.approved_intent_id, 0ULL, "approved_intent_id");
    expect_equal(plan.reservation_id, 0ULL, "reservation_id");
    expect_equal(plan.bundle_id, 0ULL, "bundle_id");
    expect_equal(plan.status, PlanStatus::Created, "status");
    expect_equal(plan.order_count, static_cast<std::uint16_t>(0), "order_count");
    expect_equal(plan.max_total_cost_tick, 0LL, "max_total_cost_tick");
    expect_equal(plan.min_expected_edge_tick, 0LL, "min_expected_edge_tick");
    expect_equal(plan.max_slippage_tick, 0LL, "max_slippage_tick");
    expect_equal(plan.chosen_bundle_qty, 0LL, "chosen_bundle_qty");
    expect_equal(plan.guaranteed_payout_tick, 0LL, "guaranteed_payout_tick");
    expect_equal(
        plan.expected_terminal_pnl_tick,
        0LL,
        "expected_terminal_pnl_tick"
    );
    expect_equal(plan.created_ts_ns, 0ULL, "created_ts_ns");
    expect_equal(plan.expire_after_ns, 0ULL, "expire_after_ns");
    expect_true(plan.idempotency_key.empty(), "idempotency_key empty");
}

void OrderPlan_RequiresReservation() {
    auto plan = valid_order_plan();
    plan.reservation_id = 0;

    const PlanValidator validator;

    expect_true(
        !validator.validate(plan, true).ok,
        "reservation required when enabled"
    );
    expect_true(
        validator.validate(plan, false).ok,
        "reservation optional when disabled"
    );
}

void OrderPlan_RejectsTooManyChildOrders() {
    auto plan = valid_order_plan();
    plan.order_count = kMaxChildOrdersPerPlan + 1;

    const PlanValidator validator;
    const auto result = validator.validate(plan);

    expect_true(!result.ok, "too many child orders rejected");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"OrderPlan_DefaultsCreated", &OrderPlan_DefaultsCreated},
        {"OrderPlan_RequiresReservation", &OrderPlan_RequiresReservation},
        {
            "OrderPlan_RejectsTooManyChildOrders",
            &OrderPlan_RejectsTooManyChildOrders
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
