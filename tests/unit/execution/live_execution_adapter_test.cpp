#include "engine/execution/adapter/LiveExecutionAdapter.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::execution::AdapterResultCode;
using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::ExecutionContext;
using trading_engine::execution::ExecutionMode;
using trading_engine::execution::ILiveOrderSigner;
using trading_engine::execution::ILiveOrderTransport;
using trading_engine::execution::LiveExecutionAdapter;
using trading_engine::execution::LiveExecutionConfig;
using trading_engine::execution::LiveOrderRequest;
using trading_engine::execution::LiveOrderSignResult;
using trading_engine::execution::LiveTransportCancelResult;
using trading_engine::execution::LiveTransportSubmitResult;
using trading_engine::execution::OrderPlan;
using trading_engine::execution::PlanStatus;
using trading_engine::execution::SignedLiveOrder;

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

ExecutionContext live_context() {
    ExecutionContext context;
    context.now_ns = 1500;
    context.config.mode = ExecutionMode::Live;
    context.config.execution_enabled = true;
    context.config.live_enabled = true;
    return context;
}

LiveExecutionConfig enabled_live_config() {
    LiveExecutionConfig config;
    config.enabled = true;
    config.max_child_notional_tick = 1000;
    return config;
}

class FakeSigner final : public ILiveOrderSigner {
public:
    bool fail = false;
    std::vector<LiveOrderRequest> requests;

    [[nodiscard]] LiveOrderSignResult sign_order(
        const LiveOrderRequest& request
    ) override {
        requests.push_back(request);
        if (fail) {
            return {
                .ok = false,
                .error = "sign failed"
            };
        }
        return {
            .ok = true,
            .order = SignedLiveOrder{
                .request_body_json = "{\"order\":\"signed\"}",
                .venue_order_id_hint = "hint-1"
            }
        };
    }
};

class FakeTransport final : public ILiveOrderTransport {
public:
    bool fail_submit = false;
    bool fail_cancel = false;
    std::vector<std::string> submitted_bodies;
    std::vector<std::string> canceled_order_ids;

    [[nodiscard]] LiveTransportSubmitResult submit_order(
        std::string_view request_body_json
    ) override {
        submitted_bodies.emplace_back(request_body_json);
        if (fail_submit) {
            return {
                .ok = false,
                .error = "submit failed"
            };
        }
        return {
            .ok = true,
            .venue_order_id = "venue-1",
            .venue_status = "live"
        };
    }

    [[nodiscard]] LiveTransportCancelResult cancel_order(
        std::string_view venue_order_id
    ) override {
        canceled_order_ids.emplace_back(venue_order_id);
        if (fail_cancel) {
            return {
                .ok = false,
                .error = "cancel failed"
            };
        }
        return {
            .ok = true
        };
    }
};

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

void LiveExecutionAdapter_RequiresExplicitLiveContext() {
    FakeSigner signer;
    FakeTransport transport;
    LiveExecutionAdapter adapter(
        enabled_live_config(),
        &signer,
        &transport
    );
    ExecutionContext context;

    const auto result = adapter.submit_plan(valid_plan(), context);

    expect_true(!result.ok, "submit rejected");
    expect_equal(
        result.code,
        AdapterResultCode::LiveExecutionDisabled,
        "disabled code"
    );
    expect_true(signer.requests.empty(), "signer unused");
    expect_true(transport.submitted_bodies.empty(), "transport unused");
}

void LiveExecutionAdapter_SubmitsSignedOrdersWhenEnabled() {
    FakeSigner signer;
    FakeTransport transport;
    LiveExecutionAdapter adapter(
        enabled_live_config(),
        &signer,
        &transport
    );

    const auto result = adapter.submit_plan(valid_plan(), live_context());
    const auto reports = adapter.poll_reports();

    expect_true(result.ok, "submit ok");
    expect_equal(result.status, PlanStatus::Acked, "plan status");
    expect_equal(result.child_orders_submitted, 1ULL, "submitted");
    expect_equal(result.child_orders_rejected, 0ULL, "rejected");
    expect_equal(result.code, AdapterResultCode::Ok, "result code");
    expect_equal(signer.requests.size(), static_cast<std::size_t>(1), "signed");
    expect_equal(
        transport.submitted_bodies.size(),
        static_cast<std::size_t>(1),
        "submitted bodies"
    );
    expect_equal(reports.size(), static_cast<std::size_t>(1), "reports");
    expect_equal(reports[0].status, ChildOrderStatus::Acked, "report status");
    expect_equal(reports[0].filled_lots, 0LL, "no fake fill");
    expect_equal(reports[0].remaining_lots, 1LL, "remaining");
    expect_equal(reports[0].venue_order_id, std::string{"venue-1"}, "venue id");
}

void LiveExecutionAdapter_RejectsMissingSigner() {
    FakeTransport transport;
    LiveExecutionAdapter adapter(
        enabled_live_config(),
        nullptr,
        &transport
    );

    const auto result = adapter.submit_plan(valid_plan(), live_context());

    expect_true(!result.ok, "submit rejected");
    expect_equal(
        result.code,
        AdapterResultCode::AdapterError,
        "adapter error"
    );
    expect_true(transport.submitted_bodies.empty(), "transport unused");
}

void LiveExecutionAdapter_CancelUsesVenueOrderIds() {
    FakeSigner signer;
    FakeTransport transport;
    LiveExecutionAdapter adapter(
        enabled_live_config(),
        &signer,
        &transport
    );

    const auto submit = adapter.submit_plan(valid_plan(), live_context());
    (void)adapter.poll_reports();
    const auto cancel = adapter.cancel_plan(submit.plan_id);

    expect_true(submit.ok, "submit ok");
    expect_true(cancel.ok, "cancel ok");
    expect_equal(cancel.code, AdapterResultCode::Ok, "cancel code");
    expect_equal(
        transport.canceled_order_ids.size(),
        static_cast<std::size_t>(1),
        "cancel count"
    );
    expect_equal(
        transport.canceled_order_ids[0],
        std::string{"venue-1"},
        "venue cancel id"
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
        },
        {
            "LiveExecutionAdapter_RequiresExplicitLiveContext",
            &LiveExecutionAdapter_RequiresExplicitLiveContext
        },
        {
            "LiveExecutionAdapter_SubmitsSignedOrdersWhenEnabled",
            &LiveExecutionAdapter_SubmitsSignedOrdersWhenEnabled
        },
        {
            "LiveExecutionAdapter_RejectsMissingSigner",
            &LiveExecutionAdapter_RejectsMissingSigner
        },
        {
            "LiveExecutionAdapter_CancelUsesVenueOrderIds",
            &LiveExecutionAdapter_CancelUsesVenueOrderIds
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
