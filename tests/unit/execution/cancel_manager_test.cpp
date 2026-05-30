#include "engine/execution/cancel/CancelManager.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::AdapterResultCode;
using trading_engine::execution::CancelManager;
using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::ExecutionConfig;
using trading_engine::execution::ExecutionMode;
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
    plan.plan_id = 300;
    plan.order_count = count;
    for (std::uint16_t i = 0; i < count; ++i) {
        auto& order = plan.orders[i];
        order.order_id = 3000 + i;
        order.plan_id = plan.plan_id;
        order.quantity_lots = i == 0 ? 10 : 5;
    }
    return plan;
}

ExecutionReport fill_report(
    std::uint64_t order_id,
    std::int64_t filled_lots,
    std::int64_t remaining_lots,
    ChildOrderStatus status
) {
    ExecutionReport report;
    report.plan_id = 300;
    report.child_order_id = order_id;
    report.status = status;
    report.filled_lots = filled_lots;
    report.remaining_lots = remaining_lots;
    report.avg_fill_price_tick = 50;
    return report;
}

void CancelManager_CancelsOpenPaperOrders() {
    const auto plan = plan_with_children(2);
    const FillTracker tracker(plan);
    ExecutionConfig config;
    config.mode = ExecutionMode::Paper;

    CancelManager manager;
    const auto result = manager.cancel_open_orders(
        plan,
        tracker.plan_state(),
        config,
        1234
    );

    expect_true(result.ok, "paper cancel ok");
    expect_equal(result.code, AdapterResultCode::Ok, "result code");
    expect_equal(result.reports.size(), static_cast<std::size_t>(4), "reports");
    expect_equal(
        result.reports[0].status,
        ChildOrderStatus::CancelRequested,
        "first cancel requested"
    );
    expect_equal(
        result.reports[1].status,
        ChildOrderStatus::Cancelled,
        "first cancelled"
    );
    expect_equal(result.reports[0].remaining_lots, 10LL, "first remaining");
    expect_equal(result.reports[1].remaining_lots, 0LL, "first cancelled remaining");
    expect_equal(result.reports[2].child_order_id, 3001ULL, "second order id");
    expect_equal(result.reports[2].remaining_lots, 5LL, "second remaining");
}

void CancelManager_DoesNotCancelFilledOrders() {
    const auto plan = plan_with_children(1);
    FillTracker tracker(plan);
    tracker.apply(fill_report(3000, 10, 0, ChildOrderStatus::Filled));
    ExecutionConfig config;
    config.mode = ExecutionMode::Paper;

    CancelManager manager;
    const auto result = manager.cancel_open_orders(
        plan,
        tracker.plan_state(),
        config,
        1234
    );

    expect_true(result.ok, "paper cancel ok");
    expect_true(result.reports.empty(), "filled order not cancelled");
}

void CancelManager_LiveDisabledReturnsUnavailable() {
    const auto plan = plan_with_children(1);
    const FillTracker tracker(plan);
    ExecutionConfig config;
    config.mode = ExecutionMode::Live;

    CancelManager manager;
    const auto result = manager.cancel_open_orders(
        plan,
        tracker.plan_state(),
        config,
        1234
    );

    expect_true(!result.ok, "live cancel rejected");
    expect_equal(
        result.code,
        AdapterResultCode::LiveExecutionDisabled,
        "live disabled code"
    );
    expect_true(result.reports.empty(), "no reports");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "CancelManager_CancelsOpenPaperOrders",
            &CancelManager_CancelsOpenPaperOrders
        },
        {
            "CancelManager_DoesNotCancelFilledOrders",
            &CancelManager_DoesNotCancelFilledOrders
        },
        {
            "CancelManager_LiveDisabledReturnsUnavailable",
            &CancelManager_LiveDisabledReturnsUnavailable
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
