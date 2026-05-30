#include "engine/execution/adapter/LiveExecutionAdapter.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::AdapterResultCode;
using trading_engine::execution::ExecutionContext;
using trading_engine::execution::LiveExecutionAdapter;
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

OrderPlan valid_plan() {
    OrderPlan plan;
    plan.plan_id = 1;
    plan.source_intent_id = 2;
    plan.approved_intent_id = 3;
    plan.reservation_id = 4;
    plan.bundle_id = 5;
    plan.order_count = 1;
    plan.max_total_cost_tick = 100;
    plan.created_ts_ns = 1000;
    plan.expire_after_ns = 2000;
    plan.idempotency_key = "intent-2:bundle-5";

    auto& order = plan.orders[0];
    order.order_id = 1;
    order.plan_id = plan.plan_id;
    order.client_order_id = "client-1";
    order.market_id = "market";
    order.asset_id = "asset";
    order.quantity_lots = 1;
    order.limit_price_tick = 10;
    order.worst_allowed_price_tick = 10;
    return plan;
}

void LiveExecutionAdapter_DisabledByDefault() {
    LiveExecutionAdapter adapter;

    const auto result = adapter.cancel_plan(1);

    expect_true(!result.ok, "cancel disabled");
    expect_equal(
        result.code,
        AdapterResultCode::LiveExecutionDisabled,
        "cancel code"
    );
}

void LiveExecutionAdapter_SubmitReturnsDisabled() {
    LiveExecutionAdapter adapter;
    ExecutionContext context;

    const auto result = adapter.submit_plan(valid_plan(), context);

    expect_true(!result.ok, "submit disabled");
    expect_equal(result.plan_id, 1ULL, "plan_id");
    expect_equal(
        result.code,
        AdapterResultCode::LiveExecutionDisabled,
        "submit code"
    );
}

void LiveExecutionAdapter_DoesNotAccessNetwork() {
    LiveExecutionAdapter adapter;
    ExecutionContext context;

    const auto reports = adapter.poll_reports();
    const auto submit = adapter.submit_plan(valid_plan(), context);
    const auto cancel = adapter.cancel_plan(1);

    expect_true(reports.empty(), "poll reports empty");
    expect_equal(
        submit.code,
        AdapterResultCode::LiveExecutionDisabled,
        "submit disabled"
    );
    expect_equal(
        cancel.code,
        AdapterResultCode::LiveExecutionDisabled,
        "cancel disabled"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "LiveExecutionAdapter_DisabledByDefault",
            &LiveExecutionAdapter_DisabledByDefault
        },
        {
            "LiveExecutionAdapter_SubmitReturnsDisabled",
            &LiveExecutionAdapter_SubmitReturnsDisabled
        },
        {
            "LiveExecutionAdapter_DoesNotAccessNetwork",
            &LiveExecutionAdapter_DoesNotAccessNetwork
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
