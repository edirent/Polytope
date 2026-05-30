#include "engine/execution/state/FillTracker.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::ExecutionReport;
using trading_engine::execution::FillTracker;
using trading_engine::execution::OrderPlan;

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

OrderPlan plan_with_children(std::uint16_t count = 2) {
    OrderPlan plan;
    plan.plan_id = 100;
    plan.order_count = count;
    for (std::uint16_t i = 0; i < count; ++i) {
        auto& order = plan.orders[i];
        order.order_id = 1000 + i;
        order.plan_id = plan.plan_id;
        order.quantity_lots = i == 0 ? 10 : 5;
    }
    return plan;
}

ExecutionReport fill_report(
    std::uint64_t order_id,
    std::int64_t filled_lots,
    std::int64_t remaining_lots,
    std::int64_t avg_price_tick,
    ChildOrderStatus status = ChildOrderStatus::Filled
) {
    ExecutionReport report;
    report.plan_id = 100;
    report.child_order_id = order_id;
    report.status = status;
    report.filled_lots = filled_lots;
    report.remaining_lots = remaining_lots;
    report.avg_fill_price_tick = avg_price_tick;
    return report;
}

void FillTracker_TracksFullFill() {
    FillTracker tracker(plan_with_children(1));

    tracker.apply(fill_report(1000, 10, 0, 40));

    const auto& plan = tracker.plan_state();
    const auto* child = tracker.find_child(1000);

    expect_true(child != nullptr, "child exists");
    expect_equal(child->requested_qty_lots, 10LL, "requested");
    expect_equal(child->filled_qty_lots, 10LL, "filled");
    expect_equal(child->remaining_qty_lots, 0LL, "remaining");
    expect_equal(child->total_cost_tick, 400LL, "child cost");
    expect_equal(child->avg_fill_price_tick, 40LL, "child avg");
    expect_true(plan.any_filled, "any filled");
    expect_true(plan.all_filled, "all filled");
    expect_true(!plan.any_partial, "no partial");
    expect_equal(plan.total_cost_tick, 400LL, "plan cost");
}

void FillTracker_TracksPartialFill() {
    FillTracker tracker(plan_with_children(1));

    tracker.apply(fill_report(
        1000,
        4,
        6,
        40,
        ChildOrderStatus::PartiallyFilled
    ));

    const auto& plan = tracker.plan_state();
    const auto* child = tracker.find_child(1000);

    expect_true(child != nullptr, "child exists");
    expect_equal(child->filled_qty_lots, 4LL, "filled");
    expect_equal(child->remaining_qty_lots, 6LL, "remaining");
    expect_equal(child->total_cost_tick, 160LL, "child cost");
    expect_true(plan.any_filled, "any filled");
    expect_true(!plan.all_filled, "not all filled");
    expect_true(plan.any_partial, "any partial");
}

void FillTracker_ComputesAveragePrice() {
    FillTracker tracker(plan_with_children(1));

    tracker.apply(fill_report(
        1000,
        4,
        6,
        40,
        ChildOrderStatus::PartiallyFilled
    ));
    tracker.apply(fill_report(1000, 6, 0, 50));

    const auto* child = tracker.find_child(1000);

    expect_true(child != nullptr, "child exists");
    expect_equal(child->filled_qty_lots, 10LL, "filled");
    expect_equal(child->total_cost_tick, 460LL, "total cost");
    expect_equal(child->avg_fill_price_tick, 46LL, "weighted average");
}

void FillTracker_DetectsAllFilled() {
    FillTracker tracker(plan_with_children(2));

    tracker.apply(fill_report(1000, 10, 0, 40));
    tracker.apply(fill_report(1001, 5, 0, 60));

    const auto& plan = tracker.plan_state();

    expect_true(plan.any_filled, "any filled");
    expect_true(plan.all_filled, "all filled");
    expect_true(!plan.any_partial, "no partial");
    expect_equal(plan.total_cost_tick, 700LL, "plan cost");
}

void FillTracker_DetectsAnyPartial() {
    FillTracker tracker(plan_with_children(2));

    tracker.apply(fill_report(1000, 10, 0, 40));
    tracker.apply(fill_report(
        1001,
        2,
        3,
        60,
        ChildOrderStatus::PartiallyFilled
    ));

    const auto& plan = tracker.plan_state();

    expect_true(plan.any_filled, "any filled");
    expect_true(!plan.all_filled, "not all filled");
    expect_true(plan.any_partial, "any partial");
    expect_equal(plan.total_cost_tick, 520LL, "plan cost");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"FillTracker_TracksFullFill", &FillTracker_TracksFullFill},
        {"FillTracker_TracksPartialFill", &FillTracker_TracksPartialFill},
        {"FillTracker_ComputesAveragePrice", &FillTracker_ComputesAveragePrice},
        {"FillTracker_DetectsAllFilled", &FillTracker_DetectsAllFilled},
        {"FillTracker_DetectsAnyPartial", &FillTracker_DetectsAnyPartial}
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
