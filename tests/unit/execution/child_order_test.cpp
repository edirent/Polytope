#include "engine/execution/plan/PlanValidator.h"
#include "engine/execution/public/ChildOrder.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::ChildOrder;
using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::OrderSide;
using trading_engine::execution::PlanValidator;

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

ChildOrder valid_child_order() {
    ChildOrder order;
    order.order_id = 1;
    order.plan_id = 2;
    order.client_order_id = "client-1";
    order.market_id = "market-1";
    order.asset_id = "asset-1";
    order.quantity_lots = 10;
    order.limit_price_tick = 100;
    order.estimated_vwap_tick = 95;
    order.worst_allowed_price_tick = 100;
    return order;
}

void ChildOrder_DefaultsCreated() {
    const ChildOrder order;

    expect_equal(order.order_id, 0ULL, "order_id");
    expect_equal(order.plan_id, 0ULL, "plan_id");
    expect_true(order.client_order_id.empty(), "client_order_id empty");
    expect_true(order.market_id.empty(), "market_id empty");
    expect_true(order.asset_id.empty(), "asset_id empty");
    expect_equal(order.side, OrderSide::Buy, "side");
    expect_equal(order.quantity_lots, 0LL, "quantity_lots");
    expect_equal(order.limit_price_tick, 0LL, "limit_price_tick");
    expect_equal(order.estimated_vwap_tick, 0LL, "estimated_vwap_tick");
    expect_equal(
        order.worst_allowed_price_tick,
        0LL,
        "worst_allowed_price_tick"
    );
    expect_equal(order.status, ChildOrderStatus::Created, "status");
}

void ChildOrder_RejectsZeroQuantity() {
    auto order = valid_child_order();
    order.quantity_lots = 0;

    const PlanValidator validator;
    const auto result = validator.validate_child_order(order);

    expect_true(!result.ok, "zero quantity rejected");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"ChildOrder_DefaultsCreated", &ChildOrder_DefaultsCreated},
        {"ChildOrder_RejectsZeroQuantity", &ChildOrder_RejectsZeroQuantity}
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
