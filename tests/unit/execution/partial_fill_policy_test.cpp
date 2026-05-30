#include "engine/execution/state/PartialFillPolicy.h"

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
using trading_engine::execution::PartialFillAction;
using trading_engine::execution::PartialFillPolicy;

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

OrderPlan plan_with_children(std::uint16_t count) {
    OrderPlan plan;
    plan.plan_id = 200;
    plan.order_count = count;
    for (std::uint16_t i = 0; i < count; ++i) {
        auto& order = plan.orders[i];
        order.order_id = 2000 + i;
        order.plan_id = plan.plan_id;
        order.quantity_lots = i == 0 ? 10 : 5;
    }
    return plan;
}

ExecutionReport report(
    std::uint64_t order_id,
    std::int64_t filled_lots,
    std::int64_t remaining_lots,
    ChildOrderStatus status
) {
    ExecutionReport report;
    report.plan_id = 200;
    report.child_order_id = order_id;
    report.status = status;
    report.filled_lots = filled_lots;
    report.remaining_lots = remaining_lots;
    report.avg_fill_price_tick = 50;
    return report;
}

void PartialFillPolicy_MultiLegPartialRequiresHedge() {
    FillTracker tracker(plan_with_children(2));
    tracker.apply(report(2000, 10, 0, ChildOrderStatus::Filled));
    tracker.apply(report(
        2001,
        2,
        3,
        ChildOrderStatus::PartiallyFilled
    ));

    const PartialFillPolicy policy;

    expect_equal(
        policy.decide(tracker.plan_state()),
        PartialFillAction::HedgeRequired,
        "multi-leg partial requires hedge"
    );
}

void PartialFillPolicy_AllFilledNoAction() {
    FillTracker tracker(plan_with_children(2));
    tracker.apply(report(2000, 10, 0, ChildOrderStatus::Filled));
    tracker.apply(report(2001, 5, 0, ChildOrderStatus::Filled));

    const PartialFillPolicy policy;

    expect_equal(
        policy.decide(tracker.plan_state()),
        PartialFillAction::None,
        "all filled no action"
    );
}

void PartialFillPolicy_NoFillNoAction() {
    FillTracker tracker(plan_with_children(2));

    const PartialFillPolicy policy;

    expect_equal(
        policy.decide(tracker.plan_state()),
        PartialFillAction::None,
        "no fill no action"
    );
}

void PartialFillPolicy_SingleLegPartialCanCancelRemaining() {
    FillTracker tracker(plan_with_children(1));
    tracker.apply(report(
        2000,
        4,
        6,
        ChildOrderStatus::PartiallyFilled
    ));

    const PartialFillPolicy policy;

    expect_equal(
        policy.decide(tracker.plan_state()),
        PartialFillAction::CancelRemaining,
        "single-leg partial cancels remaining"
    );
    expect_true(
        policy.should_cancel_remaining(tracker.state()),
        "legacy cancel helper remains true"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "PartialFillPolicy_MultiLegPartialRequiresHedge",
            &PartialFillPolicy_MultiLegPartialRequiresHedge
        },
        {
            "PartialFillPolicy_AllFilledNoAction",
            &PartialFillPolicy_AllFilledNoAction
        },
        {
            "PartialFillPolicy_NoFillNoAction",
            &PartialFillPolicy_NoFillNoAction
        },
        {
            "PartialFillPolicy_SingleLegPartialCanCancelRemaining",
            &PartialFillPolicy_SingleLegPartialCanCancelRemaining
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
