#pragma once

#include "engine/execution/adapter/IExecutionAdapter.h"
#include "engine/execution/cancel/CancelManager.h"
#include "engine/execution/core/ExecutionContext.h"
#include "engine/execution/core/ExecutionScratch.h"
#include "engine/execution/core/ExecutionPlanStore.h"
#include "engine/execution/plan/ExecutionPlanner.h"
#include "engine/execution/plan/PlanValidator.h"
#include "engine/execution/publish/ExecutionReportPublisher.h"
#include "engine/execution/publish/ReservationDispositionPublisher.h"
#include "engine/execution/public/ExecutionResult.h"
#include "engine/execution/public/OrderPlan.h"
#include "engine/execution/state/FillTracker.h"
#include "engine/execution/state/OrderStateMachine.h"
#include "engine/execution/state/PartialFillPolicy.h"
#include "engine/execution/state/PlanStateMachine.h"

#include <functional>
#include <unordered_map>
#include <vector>

namespace trading_engine::execution {

class ExecutionGateway {
public:
    explicit ExecutionGateway(
        IExecutionAdapter* adapter = nullptr,
        ExecutionReportPublisher* report_publisher = nullptr,
        ReservationDispositionPublisher* reservation_publisher = nullptr
    );

    [[nodiscard]] ExecutionResult submit_approved_intent(
        const ApprovedIntentEnvelope& envelope,
        const ExecutionContext& context
    );

    [[nodiscard]] ExecutionResult submit_approved_decision(
        const ApprovedOrderDecisionEnvelope& envelope,
        const ExecutionContext& context
    );

    [[nodiscard]] ExecutionResult submit(
        const OrderPlan& plan,
        const ExecutionContext& context
    );

    [[nodiscard]] std::vector<ExecutionReport> poll();

    [[nodiscard]] CancelResult cancel_plan(std::uint64_t plan_id);

    [[nodiscard]] const OrderPlan* find_plan(PlanId plan_id) const;

    void set_plan_observer(std::function<void(const OrderPlan&)> observer);

private:
    struct PlanRuntimeState {
        OrderPlan plan;
        FillTracker fill_tracker;
        ExecutionConfig config;
    };

    [[nodiscard]] PlanStatus apply_plan_lifecycle(
        OrderPlan* plan,
        const AdapterSubmitResult& adapter_result,
        const PlanFillState& fill_state
    ) const noexcept;

    void publish_report(const ExecutionReport& report);
    void publish_reservation_disposition(
        const ApprovedIntentEnvelope& envelope,
        PlanId plan_id,
        ReservationDispositionType type,
        const std::string& reason
    );
    void publish_reservation_disposition(
        const ApprovedOrderDecisionEnvelope& envelope,
        PlanId plan_id,
        ReservationDispositionType type,
        const std::string& reason
    );

    IExecutionAdapter* adapter_ = nullptr;
    ExecutionReportPublisher* report_publisher_ = nullptr;
    ReservationDispositionPublisher* reservation_publisher_ = nullptr;

    ExecutionPlanner planner_;
    PlanValidator validator_;
    ExecutionPlanStore plan_store_;
    OrderStateMachine order_state_machine_;
    PlanStateMachine plan_state_machine_;
    PartialFillPolicy partial_fill_policy_;
    CancelManager cancel_manager_;
    ExecutionScratch scratch_;

    std::unordered_map<PlanId, PlanRuntimeState> runtime_;
    std::vector<ExecutionReport> pending_reports_;
    std::function<void(const OrderPlan&)> plan_observer_;
};

}  // namespace trading_engine::execution
