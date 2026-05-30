#include "engine/execution/adapter/PaperExecutionAdapter.h"
#include "engine/state/MarketStateSnapshot.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::AdapterResultCode;
using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::ExecutionContext;
using trading_engine::execution::OrderPlan;
using trading_engine::execution::PaperExecutionAdapter;
using trading_engine::execution::PaperExecutionMode;
using trading_engine::execution::PlanStatus;
using trading_engine::state::MarketStateSnapshot;

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

MarketStateSnapshot snapshot(
    std::string asset_id,
    std::initializer_list<std::pair<std::int64_t, double>> asks
) {
    MarketStateSnapshot snapshot;
    snapshot.entity_id = std::move(asset_id);
    snapshot.market_id = "market";
    snapshot.live = true;
    snapshot.has_ask = asks.size() != 0;
    snapshot.usable_for_depth = true;
    snapshot.ask_count = static_cast<std::uint32_t>(asks.size());

    std::uint32_t index = 0;
    for (const auto& [price_tick, size] : asks) {
        snapshot.asks[index].price_tick = price_tick;
        snapshot.asks[index].size = size;
        ++index;
    }
    return snapshot;
}

OrderPlan plan_for_two_assets() {
    OrderPlan plan;
    plan.plan_id = 1;
    plan.source_intent_id = 2;
    plan.approved_intent_id = 3;
    plan.reservation_id = 4;
    plan.bundle_id = 5;
    plan.order_count = 2;
    plan.max_total_cost_tick = 1000;
    plan.created_ts_ns = 100;
    plan.expire_after_ns = 1000;
    plan.idempotency_key = "paper-plan";

    auto& first = plan.orders[0];
    first.order_id = 11;
    first.plan_id = plan.plan_id;
    first.client_order_id = "paper-plan-1";
    first.market_id = "market";
    first.asset_id = "asset-a";
    first.quantity_lots = 10;
    first.limit_price_tick = 50;
    first.worst_allowed_price_tick = 50;

    auto& second = plan.orders[1];
    second.order_id = 12;
    second.plan_id = plan.plan_id;
    second.client_order_id = "paper-plan-2";
    second.market_id = "market";
    second.asset_id = "asset-b";
    second.quantity_lots = 5;
    second.limit_price_tick = 60;
    second.worst_allowed_price_tick = 60;
    return plan;
}

ExecutionContext atomic_context() {
    ExecutionContext context;
    context.now_ns = 200;
    context.config.paper_mode = PaperExecutionMode::PaperAtomic;
    context.snapshots.push_back(snapshot("asset-a", {{40, 10.0}}));
    context.snapshots.push_back(snapshot("asset-b", {{55, 5.0}}));
    return context;
}

void PaperExecutionAdapter_AtomicFillsAllLegsWhenDepthEnough() {
    PaperExecutionAdapter adapter;
    const auto result = adapter.submit_plan(plan_for_two_assets(), atomic_context());
    const auto reports = adapter.poll_reports();

    expect_true(result.ok, "submit ok");
    expect_equal(result.status, PlanStatus::Filled, "plan status");
    expect_equal(result.code, AdapterResultCode::Ok, "adapter code");
    expect_equal(reports.size(), static_cast<std::size_t>(2), "report count");
    expect_equal(reports[0].status, ChildOrderStatus::Filled, "first status");
    expect_equal(reports[0].filled_lots, 10LL, "first filled");
    expect_equal(reports[0].remaining_lots, 0LL, "first remaining");
    expect_equal(reports[1].status, ChildOrderStatus::Filled, "second status");
}

void PaperExecutionAdapter_AtomicFailsAllWhenOneLegInsufficient() {
    PaperExecutionAdapter adapter;
    auto context = atomic_context();
    context.snapshots[1] = snapshot("asset-b", {{55, 1.0}});

    const auto result = adapter.submit_plan(plan_for_two_assets(), context);
    const auto reports = adapter.poll_reports();

    expect_true(!result.ok, "submit rejected");
    expect_equal(result.status, PlanStatus::Rejected, "plan status");
    expect_equal(reports.size(), static_cast<std::size_t>(2), "report count");
    expect_equal(reports[0].status, ChildOrderStatus::Rejected, "first status");
    expect_equal(reports[0].filled_lots, 0LL, "first no fill");
    expect_equal(reports[1].status, ChildOrderStatus::Rejected, "second status");
    expect_equal(reports[1].filled_lots, 0LL, "second no fill");
}

void PaperExecutionAdapter_DoesNotPartialFillInAtomicMode() {
    PaperExecutionAdapter adapter;
    auto context = atomic_context();
    context.snapshots[0] = snapshot("asset-a", {{40, 4.0}});

    const auto result = adapter.submit_plan(plan_for_two_assets(), context);
    const auto reports = adapter.poll_reports();

    expect_true(!result.ok, "atomic partial rejected");
    expect_equal(reports[0].status, ChildOrderStatus::Rejected, "first rejected");
    expect_equal(reports[0].filled_lots, 0LL, "atomic no partial fill");
}

void PaperExecutionAdapter_SequentialAllowsPartialFillWhenConfigured() {
    PaperExecutionAdapter adapter;
    auto context = atomic_context();
    context.config.paper_mode = PaperExecutionMode::PaperSequential;
    context.config.allow_partial_fill_paper = true;
    context.snapshots[0] = snapshot("asset-a", {{40, 4.0}});

    const auto result = adapter.submit_plan(plan_for_two_assets(), context);
    const auto reports = adapter.poll_reports();

    expect_true(result.ok, "sequential partial accepted");
    expect_equal(result.status, PlanStatus::PartiallyFilled, "plan status");
    expect_equal(
        reports[0].status,
        ChildOrderStatus::PartiallyFilled,
        "first partial"
    );
    expect_equal(reports[0].filled_lots, 4LL, "partial filled");
    expect_equal(reports[0].remaining_lots, 6LL, "partial remaining");
    expect_equal(reports[1].status, ChildOrderStatus::Filled, "second filled");
}

void PaperExecutionAdapter_RespectsLimitPrice() {
    PaperExecutionAdapter adapter;
    auto context = atomic_context();
    context.snapshots[0] = snapshot("asset-a", {{51, 10.0}});

    const auto result = adapter.submit_plan(plan_for_two_assets(), context);
    const auto reports = adapter.poll_reports();

    expect_true(!result.ok, "limit rejected");
    expect_equal(reports[0].status, ChildOrderStatus::Rejected, "first rejected");
    expect_equal(reports[0].filled_lots, 0LL, "no fill above limit");
}

void PaperExecutionAdapter_RejectsMissingSnapshot() {
    PaperExecutionAdapter adapter;
    auto context = atomic_context();
    context.snapshots.clear();
    context.snapshots.push_back(snapshot("asset-a", {{40, 10.0}}));

    const auto result = adapter.submit_plan(plan_for_two_assets(), context);
    const auto reports = adapter.poll_reports();

    expect_true(!result.ok, "missing snapshot rejected");
    expect_equal(reports[1].status, ChildOrderStatus::Rejected, "second rejected");
    expect_equal(
        reports[1].reject_reason,
        std::string{"MissingSnapshot"},
        "reject reason"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "PaperExecutionAdapter_AtomicFillsAllLegsWhenDepthEnough",
            &PaperExecutionAdapter_AtomicFillsAllLegsWhenDepthEnough
        },
        {
            "PaperExecutionAdapter_AtomicFailsAllWhenOneLegInsufficient",
            &PaperExecutionAdapter_AtomicFailsAllWhenOneLegInsufficient
        },
        {
            "PaperExecutionAdapter_DoesNotPartialFillInAtomicMode",
            &PaperExecutionAdapter_DoesNotPartialFillInAtomicMode
        },
        {
            "PaperExecutionAdapter_SequentialAllowsPartialFillWhenConfigured",
            &PaperExecutionAdapter_SequentialAllowsPartialFillWhenConfigured
        },
        {
            "PaperExecutionAdapter_RespectsLimitPrice",
            &PaperExecutionAdapter_RespectsLimitPrice
        },
        {
            "PaperExecutionAdapter_RejectsMissingSnapshot",
            &PaperExecutionAdapter_RejectsMissingSnapshot
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
