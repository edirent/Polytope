#include "engine/execution/core/ExecutionGateway.h"

#include <string>
#include <sstream>
#include <utility>

namespace trading_engine::execution {

ExecutionGateway::ExecutionGateway(
    IExecutionAdapter* adapter,
    ExecutionReportPublisher* report_publisher,
    ReservationDispositionPublisher* reservation_publisher
)
    : adapter_(adapter),
      report_publisher_(report_publisher),
      reservation_publisher_(reservation_publisher),
      cancel_manager_(adapter) {
    pending_reports_.reserve(kMaxChildOrdersPerPlan);
}

namespace {

bool consume_reservation(PlanStatus status, const PlanFillState& fill_state) {
    return status == PlanStatus::Filled && fill_state.all_filled;
}

bool release_reservation(PlanStatus status) {
    return status == PlanStatus::Failed ||
        status == PlanStatus::Expired ||
        status == PlanStatus::Cancelled;
}

ChildOrderStatus prepare_child_for_adapter(
    const OrderStateMachine& machine,
    ChildOrderStatus status
) noexcept {
    status = machine.transition(status, ChildOrderStatus::Planned);
    status = machine.transition(status, ChildOrderStatus::Sent);
    status = machine.transition(status, ChildOrderStatus::Acked);
    return status;
}

PlanStatus prepare_plan_for_adapter(
    const PlanStateMachine& machine,
    PlanStatus status
) noexcept {
    status = machine.transition(status, PlanStatus::Planned);
    status = machine.transition(status, PlanStatus::Sent);
    status = machine.transition(status, PlanStatus::Acked);
    return status;
}

}  // namespace

PlanResult ExecutionGateway::prepare(const ApprovedOrderDecisionEnvelope& envelope, const ExecutionContext& context) const {
    auto result = planner_.build_plan(envelope, context.now_ns, context.config);
    if (result.ok) {
        auto valid = validator_.validate(result.plan, context.config);
        if (!valid.ok) { result.ok = false; result.error = valid.error; }
    }
    return result;
}
std::string ExecutionGateway::plan_identity(const OrderPlan& p) {
    // Length-prefix strings; fixed integer separators. Includes every wire/order parameter.
    std::ostringstream out;
    auto text = [&](const std::string& v) { out << v.size() << ':' << v << ';'; };
    out << p.plan_id << ';' << p.source_intent_id << ';' << p.approved_intent_id << ';'
        << p.reservation_id << ';' << p.bundle_id << ';' << p.order_count << ';'
        << p.max_total_cost_tick << ';' << p.min_expected_edge_tick << ';' << p.max_slippage_tick << ';'
        << p.chosen_bundle_qty << ';' << p.guaranteed_payout_tick << ';' << p.expected_terminal_pnl_tick << ';'
        << p.created_ts_ns << ';' << p.expire_after_ns << ';'; text(p.idempotency_key);
    if (p.order_count > kMaxChildOrdersPerPlan) return "INVALID";
    for (std::uint16_t i=0;i<p.order_count;++i) {
        const auto& o=p.orders[i];
        out << o.order_id << ';' << o.plan_id << ';'; text(o.client_order_id); text(o.market_id); text(o.asset_id);
        out << o.market_index << ';' << o.asset_index << ';' << static_cast<int>(o.side) << ';'
            << o.quantity_lots << ';' << o.limit_price_tick << ';' << o.estimated_vwap_tick << ';' << o.worst_allowed_price_tick << ';';
    }
    return out.str();
}
ExecutionResult ExecutionGateway::submit_prepared(const OrderPlan& prepared, const ApprovedOrderDecisionEnvelope& envelope, const ExecutionContext& context) {
    const auto expected=prepare(envelope,context);
    if (!expected.ok || plan_identity(expected.plan)!=plan_identity(prepared))
        return {.ok=false,.plan_id=prepared.plan_id,.status=PlanStatus::Failed,.error="prepared plan authorization mismatch"};
    return submit_approved_decision(envelope,context);
}
void ExecutionGateway::set_before_adapter(std::function<void(const OrderPlan&)> hook) { before_adapter_=std::move(hook); }

ExecutionResult ExecutionGateway::submit_approved_intent(
    const ApprovedIntentEnvelope& envelope,
    const ExecutionContext& context
) {
    scratch_.reset();
    if (adapter_ == nullptr) {
        return {
            .ok = false,
            .status = PlanStatus::Failed,
            .error = "missing execution adapter"
        };
    }

    auto plan_result = planner_.build_plan(
        envelope,
        context.now_ns,
        context.config
    );
    if (!plan_result.ok) {
        return {
            .ok = false,
            .status = PlanStatus::Failed,
            .error = plan_result.error
        };
    }

    auto plan = std::move(plan_result.plan);
    // Stable native plan IDs are an idempotency boundary, including unknown ACK.
    // Do not send an already tracked plan to the adapter a second time.
    if (const auto* existing = plan_store_.find(plan.plan_id); existing != nullptr) {
        return {.ok = false, .plan_id = existing->plan_id, .status = existing->status,
                .error = "duplicate plan; reconcile existing order state without retry"};
    }

    const auto validation = validator_.validate(plan, context.config);
    if (!validation.ok) {
        publish_reservation_disposition(
            envelope,
            plan.plan_id,
            ReservationDispositionType::Release,
            "plan validation failed"
        );
        return {
            .ok = false,
            .plan_id = plan.plan_id,
            .status = PlanStatus::Failed,
            .error = validation.error
        };
    }

    plan.status = prepare_plan_for_adapter(plan_state_machine_, plan.status);
    for (std::uint16_t i = 0; i < plan.order_count; ++i) {
        plan.orders[i].status = prepare_child_for_adapter(
            order_state_machine_,
            plan.orders[i].status
        );
        (void)scratch_.push_child_order(plan.orders[i]);
    }
    plan_store_.put(plan);
    if (plan_observer_) {
        plan_observer_(plan);
    }

    auto fill_tracker = FillTracker(plan);
    if (before_adapter_) before_adapter_(plan);
    const auto adapter_result = adapter_->submit_plan(plan, context);
    auto reports = adapter_->poll_reports();

    for (auto& report : reports) {
        for (std::uint16_t i = 0; i < plan.order_count; ++i) {
            auto& order = plan.orders[i];
            if (order.order_id != report.child_order_id) {
                continue;
            }
            order.status = order_state_machine_.transition(
                order.status,
                report.status
            );
            report.status = order.status;
            break;
        }

        fill_tracker.apply(report);
        (void)scratch_.push_child_report(report);
        publish_report(report);
        pending_reports_.push_back(report);
    }

    plan.status = apply_plan_lifecycle(
        &plan,
        adapter_result,
        fill_tracker.plan_state()
    );

    plan_store_.put(plan);
    runtime_.insert_or_assign(
        plan.plan_id,
        PlanRuntimeState{
            .plan = plan,
            .fill_tracker = fill_tracker,
            .config = context.config
        }
    );

    if (consume_reservation(plan.status, fill_tracker.plan_state())) {
        publish_reservation_disposition(
            envelope,
            plan.plan_id,
            ReservationDispositionType::Consume,
            "paper plan filled"
        );
    } else if (release_reservation(plan.status)) {
        publish_reservation_disposition(
            envelope,
            plan.plan_id,
            ReservationDispositionType::Release,
            adapter_result.error.empty() ? "execution failed" :
                adapter_result.error
        );
    }

    return {
        .ok = adapter_result.ok &&
            plan.status != PlanStatus::Failed &&
            plan.status != PlanStatus::Expired &&
            plan.status != PlanStatus::Cancelled,
        .plan_id = plan.plan_id,
        .status = plan.status,
        .child_orders_submitted = adapter_result.child_orders_submitted,
        .child_orders_rejected = adapter_result.child_orders_rejected,
        .error = adapter_result.error
    };
}

ExecutionResult ExecutionGateway::submit_approved_decision(
    const ApprovedOrderDecisionEnvelope& envelope,
    const ExecutionContext& context
) {
    scratch_.reset();
    if (adapter_ == nullptr) {
        return {
            .ok = false,
            .status = PlanStatus::Failed,
            .error = "missing execution adapter"
        };
    }

    auto plan_result = planner_.build_plan(
        envelope,
        context.now_ns,
        context.config
    );
    if (!plan_result.ok) {
        return {
            .ok = false,
            .status = PlanStatus::Failed,
            .error = plan_result.error
        };
    }

    auto plan = std::move(plan_result.plan);
    // Stable native plan IDs are an idempotency boundary, including unknown ACK.
    // Do not send an already tracked plan to the adapter a second time.
    if (const auto* existing = plan_store_.find(plan.plan_id); existing != nullptr) {
        return {.ok = false, .plan_id = existing->plan_id, .status = existing->status,
                .error = "duplicate plan; reconcile existing order state without retry"};
    }

    const auto validation = validator_.validate(plan, context.config);
    if (!validation.ok) {
        publish_reservation_disposition(
            envelope,
            plan.plan_id,
            ReservationDispositionType::Release,
            "plan validation failed"
        );
        return {
            .ok = false,
            .plan_id = plan.plan_id,
            .status = PlanStatus::Failed,
            .error = validation.error
        };
    }

    plan.status = prepare_plan_for_adapter(plan_state_machine_, plan.status);
    for (std::uint16_t i = 0; i < plan.order_count; ++i) {
        plan.orders[i].status = prepare_child_for_adapter(
            order_state_machine_,
            plan.orders[i].status
        );
        (void)scratch_.push_child_order(plan.orders[i]);
    }
    plan_store_.put(plan);
    if (plan_observer_) {
        plan_observer_(plan);
    }

    auto fill_tracker = FillTracker(plan);
    if (before_adapter_) before_adapter_(plan);
    const auto adapter_result = adapter_->submit_plan(plan, context);
    auto reports = adapter_->poll_reports();

    for (auto& report : reports) {
        for (std::uint16_t i = 0; i < plan.order_count; ++i) {
            auto& order = plan.orders[i];
            if (order.order_id != report.child_order_id) {
                continue;
            }
            order.status = order_state_machine_.transition(
                order.status,
                report.status
            );
            report.status = order.status;
            break;
        }

        fill_tracker.apply(report);
        (void)scratch_.push_child_report(report);
        publish_report(report);
        pending_reports_.push_back(report);
    }

    plan.status = apply_plan_lifecycle(
        &plan,
        adapter_result,
        fill_tracker.plan_state()
    );

    plan_store_.put(plan);
    runtime_.insert_or_assign(
        plan.plan_id,
        PlanRuntimeState{
            .plan = plan,
            .fill_tracker = fill_tracker,
            .config = context.config
        }
    );

    if (consume_reservation(plan.status, fill_tracker.plan_state())) {
        publish_reservation_disposition(
            envelope,
            plan.plan_id,
            ReservationDispositionType::Consume,
            "paper plan filled"
        );
    } else if (release_reservation(plan.status)) {
        publish_reservation_disposition(
            envelope,
            plan.plan_id,
            ReservationDispositionType::Release,
            adapter_result.error.empty() ? "execution failed" :
                adapter_result.error
        );
    }

    return {
        .ok = adapter_result.ok &&
            plan.status != PlanStatus::Failed &&
            plan.status != PlanStatus::Expired &&
            plan.status != PlanStatus::Cancelled,
        .plan_id = plan.plan_id,
        .status = plan.status,
        .child_orders_submitted = adapter_result.child_orders_submitted,
        .child_orders_rejected = adapter_result.child_orders_rejected,
        .error = adapter_result.error
    };
}

ExecutionResult ExecutionGateway::submit(
    const OrderPlan& plan,
    const ExecutionContext& context
) {
    if (adapter_ == nullptr) {
        return {
            .ok = false,
            .plan_id = plan.plan_id,
            .status = PlanStatus::Rejected,
            .error = "missing execution adapter"
        };
    }
    if (before_adapter_) before_adapter_(plan);
    const auto result = adapter_->submit_plan(plan, context);
    return {
        .ok = result.ok,
        .plan_id = result.plan_id,
        .status = result.status,
        .child_orders_submitted = result.child_orders_submitted,
        .child_orders_rejected = result.child_orders_rejected,
        .error = result.error
    };
}

std::vector<ExecutionReport> ExecutionGateway::poll() {
    auto reports = std::move(pending_reports_);
    pending_reports_.clear();
    return reports;
}

CancelResult ExecutionGateway::cancel_plan(std::uint64_t plan_id) {
    const auto it = runtime_.find(plan_id);
    if (it == runtime_.end()) {
        return {
            .ok = false,
            .plan_id = plan_id,
            .code = AdapterResultCode::AdapterError,
            .error = "unknown plan_id"
        };
    }

    auto& runtime = it->second;
    const auto cancel = cancel_manager_.cancel_open_orders(
        runtime.plan,
        runtime.fill_tracker.plan_state(),
        runtime.config
    );

    for (const auto& report : cancel.reports) {
        publish_report(report);
        pending_reports_.push_back(report);
    }

    if (cancel.ok) {
        runtime.plan.status = PlanStatus::Cancelled;
    }

    return {
        .ok = cancel.ok,
        .plan_id = cancel.plan_id,
        .code = cancel.code,
        .reports = cancel.reports,
        .error = cancel.error
    };
}

const OrderPlan* ExecutionGateway::find_plan(PlanId plan_id) const {
    return plan_store_.find(plan_id);
}

void ExecutionGateway::set_plan_observer(
    std::function<void(const OrderPlan&)> observer
) {
    plan_observer_ = std::move(observer);
}

PlanStatus ExecutionGateway::apply_plan_lifecycle(
    OrderPlan*,
    const AdapterSubmitResult& adapter_result,
    const PlanFillState& fill_state
) const noexcept {
    PlanStatus status = PlanStatus::Acked;
    const auto partial_action = partial_fill_policy_.decide(fill_state);

    if (partial_action == PartialFillAction::HedgeRequired) {
        status = plan_state_machine_.transition(
            status,
            PlanStatus::PartiallyFilled
        );
        return plan_state_machine_.transition(status, PlanStatus::HedgeRequired);
    }

    if (adapter_result.status == PlanStatus::Filled && fill_state.all_filled) {
        return plan_state_machine_.transition(status, PlanStatus::Filled);
    }

    if (adapter_result.status == PlanStatus::PartiallyFilled ||
        fill_state.any_partial) {
        return plan_state_machine_.transition(
            status,
            PlanStatus::PartiallyFilled
        );
    }

    if (!adapter_result.ok ||
        adapter_result.status == PlanStatus::Failed ||
        adapter_result.child_orders_rejected > 0) {
        return plan_state_machine_.transition(status, PlanStatus::Failed);
    }

    return adapter_result.status;
}

void ExecutionGateway::publish_report(const ExecutionReport& report) {
    if (report_publisher_ != nullptr) {
        report_publisher_->publish(report);
    }
}

void ExecutionGateway::publish_reservation_disposition(
    const ApprovedIntentEnvelope& envelope,
    PlanId plan_id,
    ReservationDispositionType type,
    const std::string& reason
) {
    if (reservation_publisher_ == nullptr ||
        type == ReservationDispositionType::None) {
        return;
    }

    reservation_publisher_->publish({
        .reservation_id = std::to_string(envelope.approval.reservation_id),
        .plan_id = plan_id,
        .type = type,
        .reason = reason
    });
}

void ExecutionGateway::publish_reservation_disposition(
    const ApprovedOrderDecisionEnvelope& envelope,
    PlanId plan_id,
    ReservationDispositionType type,
    const std::string& reason
) {
    if (reservation_publisher_ == nullptr ||
        type == ReservationDispositionType::None) {
        return;
    }

    const auto reservation_id =
        !envelope.approved.reservation_id.empty()
            ? envelope.approved.reservation_id
            : std::to_string(envelope.approved.reservation_id_hash);

    reservation_publisher_->publish({
        .reservation_id = reservation_id,
        .plan_id = plan_id,
        .type = type,
        .reason = reason
    });
}

}  // namespace trading_engine::execution
