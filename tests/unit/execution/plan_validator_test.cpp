#include "engine/execution/plan/PlanValidator.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::ExecutionConfig;
using trading_engine::execution::OrderPlan;
using trading_engine::execution::PlanValidator;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

OrderPlan valid_plan() {
    OrderPlan plan;
    plan.plan_id = 1;
    plan.source_intent_id = 2;
    plan.approved_intent_id = 3;
    plan.reservation_id = 4;
    plan.bundle_id = 5;
    plan.order_count = 2;
    plan.max_total_cost_tick = 1000;
    plan.min_expected_edge_tick = 50;
    plan.max_slippage_tick = 5;
    plan.created_ts_ns = 1000;
    plan.expire_after_ns = 1500;
    plan.idempotency_key = "intent-2:bundle-5";

    auto& first = plan.orders[0];
    first.order_id = 11;
    first.plan_id = plan.plan_id;
    first.client_order_id = "client-1";
    first.market_id = "market-a";
    first.asset_id = "asset-a";
    first.quantity_lots = 10;
    first.limit_price_tick = 40;
    first.estimated_vwap_tick = 35;
    first.worst_allowed_price_tick = 40;

    auto& second = plan.orders[1];
    second.order_id = 12;
    second.plan_id = plan.plan_id;
    second.client_order_id = "client-2";
    second.market_id = "market-b";
    second.asset_id = "asset-b";
    second.quantity_lots = 20;
    second.limit_price_tick = 60;
    second.estimated_vwap_tick = 55;
    second.worst_allowed_price_tick = 60;
    return plan;
}

ExecutionConfig valid_config() {
    ExecutionConfig config;
    config.max_child_orders_per_plan = 16;
    config.max_order_age_ns = 1'000;
    return config;
}

void PlanValidator_AcceptsValidPlan() {
    const PlanValidator validator;
    const auto result = validator.validate(valid_plan(), valid_config());

    expect_true(result.ok, "valid plan accepted");
}

void PlanValidator_RejectsMissingCostLimits() {
    auto plan = valid_plan();
    plan.max_total_cost_tick = 0;

    const PlanValidator validator;
    const auto result = validator.validate(plan, valid_config());

    expect_true(!result.ok, "missing cost limit rejected");
}

void PlanValidator_RejectsTtlBeyondConfig() {
    auto plan = valid_plan();
    plan.expire_after_ns = 3000;

    const PlanValidator validator;
    const auto result = validator.validate(plan, valid_config());

    expect_true(!result.ok, "ttl beyond config rejected");
}

void PlanValidator_RejectsMissingReservationWithConfig() {
    auto plan = valid_plan();
    plan.reservation_id = 0;

    const PlanValidator validator;
    const auto result = validator.validate(plan, valid_config());

    expect_true(!result.ok, "missing reservation rejected");
}

void PlanValidator_RejectsDuplicateClientOrderId() {
    auto plan = valid_plan();
    plan.orders[1].client_order_id = plan.orders[0].client_order_id;

    const PlanValidator validator;
    const auto result = validator.validate(plan, valid_config());

    expect_true(!result.ok, "duplicate client_order_id rejected");
}

void PlanValidator_RejectsInvalidChildOrder() {
    auto plan = valid_plan();
    plan.orders[0].quantity_lots = 0;

    const PlanValidator validator;
    const auto result = validator.validate(plan, valid_config());

    expect_true(!result.ok, "invalid child rejected");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"PlanValidator_AcceptsValidPlan", &PlanValidator_AcceptsValidPlan},
        {
            "PlanValidator_RejectsMissingCostLimits",
            &PlanValidator_RejectsMissingCostLimits
        },
        {
            "PlanValidator_RejectsTtlBeyondConfig",
            &PlanValidator_RejectsTtlBeyondConfig
        },
        {
            "PlanValidator_RejectsMissingReservationWithConfig",
            &PlanValidator_RejectsMissingReservationWithConfig
        },
        {
            "PlanValidator_RejectsDuplicateClientOrderId",
            &PlanValidator_RejectsDuplicateClientOrderId
        },
        {
            "PlanValidator_RejectsInvalidChildOrder",
            &PlanValidator_RejectsInvalidChildOrder
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
