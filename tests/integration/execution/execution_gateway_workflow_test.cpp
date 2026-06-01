#include "engine/execution/adapter/PaperExecutionAdapter.h"
#include "engine/execution/core/ExecutionGateway.h"
#include "engine/execution/publish/CapturingExecutionPublisher.h"
#include "engine/execution/publish/ReservationDispositionPublisher.h"
#include "engine/state/MarketStateSnapshot.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::execution::AdapterResultCode;
using trading_engine::execution::ApprovedIntentEnvelope;
using trading_engine::execution::ApprovedOrderDecisionEnvelope;
using trading_engine::execution::CapturingExecutionPublisher;
using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::ExecutionApproval;
using trading_engine::execution::ExecutionContext;
using trading_engine::execution::ExecutionGateway;
using trading_engine::execution::ExecutionReport;
using trading_engine::execution::PaperExecutionAdapter;
using trading_engine::execution::PaperExecutionMode;
using trading_engine::execution::PlanStatus;
using trading_engine::execution::ReservationDisposition;
using trading_engine::execution::ReservationDispositionPublisher;
using trading_engine::execution::ReservationDispositionType;
using trading_engine::order_decision::compute_order_decision_hash;
using trading_engine::order_decision::make_approved_order_decision_envelope;
using trading_engine::order_decision::OrderDecision;
using trading_engine::order_decision::OrderDecisionType;
using trading_engine::risk::make_approved_decision;
using trading_engine::signal::IntentStatus;
using trading_engine::signal::OpportunityIntent;
using trading_engine::signal::Side;
using trading_engine::state::MarketStateSnapshot;

class CapturingReservationPublisher final : public ReservationDispositionPublisher {
public:
    void publish(const ReservationDisposition& disposition) override {
        dispositions_.push_back(disposition);
    }

    [[nodiscard]] const std::vector<ReservationDisposition>& dispositions()
        const noexcept {
        return dispositions_;
    }

private:
    std::vector<ReservationDisposition> dispositions_;
};

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
    snapshot.usable_for_depth = true;
    snapshot.has_ask = asks.size() != 0;
    snapshot.ask_count = static_cast<std::uint32_t>(asks.size());

    std::uint32_t index = 0;
    for (const auto& [price_tick, size] : asks) {
        snapshot.asks[index].price_tick = price_tick;
        snapshot.asks[index].size = size;
        ++index;
    }
    return snapshot;
}

ApprovedIntentEnvelope envelope() {
    ApprovedIntentEnvelope envelope;
    auto& intent = envelope.source_intent;
    intent.intent_id = 101;
    intent.bundle_id = 202;
    intent.status = IntentStatus::PaperOpportunity;
    intent.estimated_cost_tick = 1000;
    intent.total_edge_tick = 100;
    intent.expires_at_ns = 2000;
    intent.idempotency_key = "intent-101:bundle-202";
    intent.leg_count = 2;

    auto& first = intent.legs[0];
    first.market_id = "market";
    first.asset_id = "asset-a";
    first.side = Side::Buy;
    first.quantity_lots = 10;
    first.estimated_vwap_tick = 40;
    first.worst_price_tick = 50;
    first.estimated_cost_tick = 400;

    auto& second = intent.legs[1];
    second.market_id = "market";
    second.asset_id = "asset-b";
    second.side = Side::Buy;
    second.quantity_lots = 5;
    second.estimated_vwap_tick = 55;
    second.worst_price_tick = 60;
    second.estimated_cost_tick = 275;

    auto& approval = envelope.approval;
    approval.decision_id = 303;
    approval.reservation_id = 404;
    approval.bundle_id = intent.bundle_id;
    approval.approved_bundle_qty = 1;
    approval.idempotency_key = intent.idempotency_key;
    return envelope;
}

ApprovedOrderDecisionEnvelope decision_envelope() {
    const auto source = envelope().source_intent;

    OrderDecision decision;
    decision.source_intent_id = source.intent_id;
    decision.bundle_id = source.bundle_id;
    decision.type = OrderDecisionType::PaperOrderDecision;
    decision.chosen_bundle_qty = 1;
    decision.estimated_total_cost_tick = source.estimated_cost_tick;
    decision.total_edge_tick = source.total_edge_tick;
    decision.expires_at_ns = source.expires_at_ns;
    decision.leg_count = source.leg_count;
    for (std::uint16_t i = 0; i < source.leg_count; ++i) {
        decision.legs[i].market_id = source.legs[i].market_id;
        decision.legs[i].asset_id = source.legs[i].asset_id;
        decision.legs[i].side = source.legs[i].side;
        decision.legs[i].quantity_lots = source.legs[i].quantity_lots;
        decision.legs[i].estimated_vwap_tick =
            source.legs[i].estimated_vwap_tick;
        decision.legs[i].worst_price_tick = source.legs[i].worst_price_tick;
        decision.legs[i].limit_price_tick = source.legs[i].worst_price_tick;
        decision.legs[i].estimated_cost_tick =
            source.legs[i].estimated_cost_tick;
    }
    decision.decision_hash = compute_order_decision_hash(decision);
    decision.decision_id = decision.decision_hash;

    trading_engine::risk::ApprovedIntent approved;
    approved.intent = source;
    approved.decision = make_approved_decision(1, 2);
    approved.decision.decision_id = 303;
    approved.decision.intent_id = source.intent_id;
    approved.decision.bundle_id = source.bundle_id;
    approved.reservation_id = "404";
    approved.reservation_id_hash = 404;
    approved.approved_at_ns = 1000;
    approved.expires_at_ns = source.expires_at_ns;

    return make_approved_order_decision_envelope(
        approved,
        decision,
        1000
    );
}

ExecutionContext context_with_depth(bool enough_second_leg = true) {
    ExecutionContext context;
    context.now_ns = 1000;
    context.config.paper_mode = PaperExecutionMode::PaperAtomic;
    context.config.max_order_age_ns = 10'000;
    context.snapshots.push_back(snapshot("asset-a", {{40, 10.0}}));
    context.snapshots.push_back(snapshot(
        "asset-b",
        {{55, enough_second_leg ? 5.0 : 1.0}}
    ));
    return context;
}

void ExecutionGateway_FillsPaperAtomicPlan() {
    PaperExecutionAdapter adapter;
    CapturingExecutionPublisher report_publisher;
    CapturingReservationPublisher reservation_publisher;
    ExecutionGateway gateway(&adapter, &report_publisher, &reservation_publisher);

    const auto result = gateway.submit_approved_intent(
        envelope(),
        context_with_depth()
    );
    const auto reports = gateway.poll();

    expect_true(result.ok, "result ok");
    expect_equal(result.status, PlanStatus::Filled, "plan filled");
    expect_equal(result.child_orders_submitted, 2ULL, "submitted");
    expect_equal(reports.size(), static_cast<std::size_t>(2), "report count");
    expect_equal(reports[0].status, ChildOrderStatus::Filled, "first filled");
    expect_equal(reports[1].status, ChildOrderStatus::Filled, "second filled");
    expect_equal(report_publisher.reports().size(), reports.size(), "published");
}

void ExecutionGateway_FillsApprovedOrderDecisionPlan() {
    PaperExecutionAdapter adapter;
    CapturingExecutionPublisher report_publisher;
    CapturingReservationPublisher reservation_publisher;
    ExecutionGateway gateway(&adapter, &report_publisher, &reservation_publisher);

    const auto result = gateway.submit_approved_decision(
        decision_envelope(),
        context_with_depth()
    );
    const auto reports = gateway.poll();

    expect_true(result.ok, "result ok");
    expect_equal(result.status, PlanStatus::Filled, "plan filled");
    expect_equal(result.child_orders_submitted, 2ULL, "submitted");
    expect_equal(reports.size(), static_cast<std::size_t>(2), "report count");
    expect_equal(
        reservation_publisher.dispositions()[0].type,
        ReservationDispositionType::Consume,
        "consume disposition"
    );
}

void ExecutionGateway_FailsWhenOneLegInsufficient() {
    PaperExecutionAdapter adapter;
    CapturingExecutionPublisher report_publisher;
    CapturingReservationPublisher reservation_publisher;
    ExecutionGateway gateway(&adapter, &report_publisher, &reservation_publisher);

    const auto result = gateway.submit_approved_intent(
        envelope(),
        context_with_depth(false)
    );
    const auto reports = gateway.poll();

    expect_true(!result.ok, "result rejected");
    expect_equal(result.status, PlanStatus::Failed, "plan failed");
    expect_equal(result.child_orders_rejected, 2ULL, "rejected");
    expect_equal(reports.size(), static_cast<std::size_t>(2), "report count");
    expect_equal(reports[0].status, ChildOrderStatus::Failed, "first failed");
    expect_equal(reports[1].status, ChildOrderStatus::Failed, "second failed");
}

void ExecutionGateway_GeneratesReleaseDispositionOnFailure() {
    PaperExecutionAdapter adapter;
    CapturingExecutionPublisher report_publisher;
    CapturingReservationPublisher reservation_publisher;
    ExecutionGateway gateway(&adapter, &report_publisher, &reservation_publisher);

    const auto result = gateway.submit_approved_intent(
        envelope(),
        context_with_depth(false)
    );

    expect_true(!result.ok, "result rejected");
    expect_equal(
        reservation_publisher.dispositions().size(),
        static_cast<std::size_t>(1),
        "disposition count"
    );
    expect_equal(
        reservation_publisher.dispositions()[0].type,
        ReservationDispositionType::Release,
        "release disposition"
    );
    expect_equal(
        reservation_publisher.dispositions()[0].reservation_id,
        std::string{"404"},
        "reservation id"
    );
}

void ExecutionGateway_GeneratesConsumeDispositionOnFill() {
    PaperExecutionAdapter adapter;
    CapturingExecutionPublisher report_publisher;
    CapturingReservationPublisher reservation_publisher;
    ExecutionGateway gateway(&adapter, &report_publisher, &reservation_publisher);

    const auto result = gateway.submit_approved_intent(
        envelope(),
        context_with_depth()
    );

    expect_true(result.ok, "result ok");
    expect_equal(
        reservation_publisher.dispositions().size(),
        static_cast<std::size_t>(1),
        "disposition count"
    );
    expect_equal(
        reservation_publisher.dispositions()[0].type,
        ReservationDispositionType::Consume,
        "consume disposition"
    );
}

void ExecutionGateway_MultiLegPartialGoesHedgeRequiredInSequentialMode() {
    PaperExecutionAdapter adapter;
    CapturingExecutionPublisher report_publisher;
    CapturingReservationPublisher reservation_publisher;
    ExecutionGateway gateway(&adapter, &report_publisher, &reservation_publisher);

    auto context = context_with_depth();
    context.config.paper_mode = PaperExecutionMode::PaperSequential;
    context.config.allow_partial_fill_paper = true;
    context.snapshots[0] = snapshot("asset-a", {{40, 4.0}});

    const auto result = gateway.submit_approved_intent(envelope(), context);
    const auto reports = gateway.poll();

    expect_true(result.ok, "partial submit accepted");
    expect_equal(result.status, PlanStatus::HedgeRequired, "hedge required");
    expect_equal(reports.size(), static_cast<std::size_t>(2), "report count");
    expect_equal(
        reports[0].status,
        ChildOrderStatus::PartiallyFilled,
        "first partial"
    );
    expect_equal(reports[1].status, ChildOrderStatus::Filled, "second filled");
    expect_true(
        reservation_publisher.dispositions().empty(),
        "no terminal disposition"
    );
}

void ExecutionGateway_DeterministicReports() {
    PaperExecutionAdapter first_adapter;
    CapturingExecutionPublisher first_report_publisher;
    CapturingReservationPublisher first_reservation_publisher;
    ExecutionGateway first_gateway(
        &first_adapter,
        &first_report_publisher,
        &first_reservation_publisher
    );

    PaperExecutionAdapter second_adapter;
    CapturingExecutionPublisher second_report_publisher;
    CapturingReservationPublisher second_reservation_publisher;
    ExecutionGateway second_gateway(
        &second_adapter,
        &second_report_publisher,
        &second_reservation_publisher
    );

    const auto first_result = first_gateway.submit_approved_intent(
        envelope(),
        context_with_depth()
    );
    const auto first_reports = first_gateway.poll();
    const auto second_result = second_gateway.submit_approved_intent(
        envelope(),
        context_with_depth()
    );
    const auto second_reports = second_gateway.poll();

    expect_equal(first_result.plan_id, second_result.plan_id, "plan id");
    expect_equal(first_reports.size(), second_reports.size(), "report count");
    for (std::size_t i = 0; i < first_reports.size(); ++i) {
        expect_equal(
            first_reports[i].child_order_id,
            second_reports[i].child_order_id,
            "child_order_id"
        );
        expect_equal(first_reports[i].status, second_reports[i].status, "status");
        expect_equal(
            first_reports[i].filled_lots,
            second_reports[i].filled_lots,
            "filled"
        );
        expect_equal(
            first_reports[i].remaining_lots,
            second_reports[i].remaining_lots,
            "remaining"
        );
        expect_equal(
            first_reports[i].avg_fill_price_tick,
            second_reports[i].avg_fill_price_tick,
            "avg price"
        );
    }
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "ExecutionGateway_FillsPaperAtomicPlan",
            &ExecutionGateway_FillsPaperAtomicPlan
        },
        {
            "ExecutionGateway_FillsApprovedOrderDecisionPlan",
            &ExecutionGateway_FillsApprovedOrderDecisionPlan
        },
        {
            "ExecutionGateway_FailsWhenOneLegInsufficient",
            &ExecutionGateway_FailsWhenOneLegInsufficient
        },
        {
            "ExecutionGateway_GeneratesReleaseDispositionOnFailure",
            &ExecutionGateway_GeneratesReleaseDispositionOnFailure
        },
        {
            "ExecutionGateway_GeneratesConsumeDispositionOnFill",
            &ExecutionGateway_GeneratesConsumeDispositionOnFill
        },
        {
            "ExecutionGateway_MultiLegPartialGoesHedgeRequiredInSequentialMode",
            &ExecutionGateway_MultiLegPartialGoesHedgeRequiredInSequentialMode
        },
        {
            "ExecutionGateway_DeterministicReports",
            &ExecutionGateway_DeterministicReports
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
