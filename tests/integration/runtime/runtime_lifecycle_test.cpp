#include "engine/execution/core/ExecutionGateway.h"
#include "engine/execution/state/FillTracker.h"
#include "engine/execution/cancel/CancelManager.h"
#include "engine/execution/publish/ReservationDispositionPublisher.h"
#include "engine/paper/ledger/PaperLedger.h"
#include <stdexcept>
#include "engine/memory/RingBuffer.h"
#include <iostream>
using namespace trading_engine;
void check(bool yes,const char* why) { if(!yes) throw std::runtime_error(why); }
struct FaultAdapter final : execution::IExecutionAdapter {
    std::size_t sends=0;
    std::vector<execution::ExecutionReport> reports;
    execution::AdapterSubmitResult submit_plan(const execution::OrderPlan& p,const execution::ExecutionContext& ctx) override {
        ++sends;
        // One partial delta, delivered twice. Unknown remaining acknowledgment.
        reports.push_back({.plan_id=p.plan_id,.child_order_id=p.orders[0].order_id,
            .status=execution::ChildOrderStatus::PartiallyFilled,.filled_lots=2,.remaining_lots=8,
            .avg_fill_price_tick=40,.event_ts_ns=ctx.now_ns});
        reports.push_back(reports.back());
        return {.ok=true,.plan_id=p.plan_id,.status=execution::PlanStatus::PartiallyFilled,.child_orders_submitted=2};
    }
    std::vector<execution::ExecutionReport> poll_reports() override { auto r=reports;reports.clear();return r; }
    execution::AdapterCancelResult cancel_plan(std::uint64_t) override { return {.ok=false,.error="no authoritative cancellation"}; }
};
struct Dispositions final : execution::ReservationDispositionPublisher {
    std::size_t releases=0;
    void publish(const execution::ReservationDisposition& d) override { if(d.type==execution::ReservationDispositionType::Release || d.type==execution::ReservationDispositionType::Consume) ++releases; }
};
execution::ApprovedIntentEnvelope envelope() {
    execution::ApprovedIntentEnvelope e; auto& i=e.source_intent;
    i.intent_id=101;i.bundle_id=202;i.status=signal::IntentStatus::PaperOpportunity;
    i.estimated_cost_tick=900;i.total_edge_tick=100;i.created_ts_ns=1000;i.expires_at_ns=3000;
    i.idempotency_key="runtime-test-101";i.leg_count=2;
    for(int n=0;n<2;++n) {
        auto& l=i.legs[n];l.market_id="m";l.asset_id="a"+std::to_string(n);l.quantity_lots=1;
        l.estimated_vwap_tick=40;l.worst_price_tick=50;l.side=signal::Side::Buy;
    }
    e.approval.decision_id=303;e.approval.reservation_id=404;e.approval.bundle_id=202;
    e.approval.approved_bundle_qty=10;e.approval.idempotency_key=i.idempotency_key;return e;
}
int main() {
 try {
    FaultAdapter adapter; Dispositions dispositions;
    execution::ExecutionGateway oms(&adapter,nullptr,&dispositions);
    execution::ExecutionContext ctx;ctx.now_ns=1500;ctx.config.execution_enabled=true;
    ctx.config.mode=execution::ExecutionMode::Paper;
    auto first=oms.submit_approved_intent(envelope(),ctx);
    check(first.status==execution::PlanStatus::HedgeRequired || first.status==execution::PlanStatus::PartiallyFilled,"partial lifecycle");
    check(dispositions.releases==0,"partial risk reservation prematurely released");
    auto again=oms.submit_approved_intent(envelope(),ctx);
    check(!again.ok && adapter.sends==1,"duplicate intent retried at adapter");
    auto cancel=oms.cancel_plan(first.plan_id);
    check(cancel.ok,"paper cancellation must remain simulated");
    execution::ExecutionConfig disabled_live; disabled_live.mode=execution::ExecutionMode::Live;
    execution::FillTracker cancellation_state(*oms.find_plan(first.plan_id));
    auto external=execution::CancelManager(&adapter).cancel_open_orders(*oms.find_plan(first.plan_id),cancellation_state.plan_state(),disabled_live);
    check(!external.ok && external.reports.empty(),"external cancellation claimed without authority");
    check(dispositions.releases==0,"unknown cancellation released risk");
    execution::OrderPlan p; p.plan_id=10;p.order_count=1;p.orders[0].order_id=11;p.orders[0].quantity_lots=10;
    execution::FillTracker tracker(p);
    execution::ExecutionReport report{.plan_id=10,.child_order_id=11,.status=execution::ChildOrderStatus::PartiallyFilled,
        .filled_lots=3,.remaining_lots=7,.avg_fill_price_tick=40,.event_ts_ns=1000};
    tracker.apply(report);tracker.apply(report);
    check(tracker.state().filled_lots==3 && tracker.plan_state().total_cost_tick==120,"duplicate fill inflated OMS totals");
    paper::PaperLedger ledger(10000); paper::FillApplication fill;
    fill.report=report;fill.asset_id="a";fill.side=execution::OrderSide::Buy;
    check(ledger.apply_fill(fill).applied,"initial partial fill");
    check(!ledger.apply_fill(fill).applied,"duplicate fill applied to ledger");
    report.filled_lots=7;report.remaining_lots=0;report.status=execution::ChildOrderStatus::Filled;report.event_ts_ns=1100;
    tracker.apply(report);fill.report=report;check(ledger.apply_fill(fill).applied,"second partial delta not applied");
    check(tracker.plan_state().all_filled && tracker.state().filled_lots==10,"partial completion");
    check(ledger.cash_ledger().cash_tick==9600 && ledger.position_ledger().lots("a")==10,"accounting reconciliation");
    report.filled_lots=1;report.event_ts_ns=1200;tracker.apply(report);
    check(tracker.state().filled_lots==10,"overfill accepted");
    execution::FillTracker foreign_tracker(p);
    report.plan_id=999; foreign_tracker.apply(report);
    check(foreign_tracker.state().filled_lots==0,"foreign plan report accepted");
    memory::RingBuffer<int> queue(2);
    check(queue.push_back(1) && queue.push_back(2) && !queue.push_back(3),"queue capacity not enforced");
    check(queue.front()==1,"queue ordering");queue.pop_front();check(queue.front()==2,"queue FIFO");
    paper::PaperLedger fee_ledger(10000);
    fill.report.plan_id=10;fill.report.filled_lots=3;fill.report.avg_fill_price_tick=40;
    fill.report.fill_id="SIMULATED:fee-receipt";fill.fee_tick=3;
    check(fee_ledger.apply_fill(fill).applied,"fee-bearing receipt rejected");
    check(fee_ledger.cash_ledger().cash_tick==9877 && fee_ledger.cash_ledger().fees_paid_tick==3,"separate fee posting");
    check(!fee_ledger.apply_fill(fill).applied,"duplicate fee posted");
    fill.fee_tick=4;auto conflicting=fee_ledger.apply_fill(fill);
    check(conflicting.status==paper::PaperLedgerApplyStatus::InvalidFill && fee_ledger.cash_ledger().cash_tick==9877,"conflicting receipt silently accepted");
    std::cout<<"runtime lifecycle: 21 assertions passed\n"; return 0;
 } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
