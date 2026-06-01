#pragma once

#include "engine/risk/core/RiskContext.h"
#include "engine/risk/core/RiskPipeline.h"
#include "engine/risk/ledger/ReservationBook.h"
#include "engine/order_decision/public/OrderDecision.h"
#include "engine/risk/public/RiskInputView.h"

namespace trading_engine::risk {

class RiskEngine {
public:
    RiskEngine();
    explicit RiskEngine(RiskRuntimeContext runtime_context);

    [[nodiscard]] RiskPipelineResult evaluate(
        const signal::OpportunityIntent& intent,
        const RiskEvaluationContext& context
    );

    [[nodiscard]] RiskPipelineResult evaluate(
        const RiskInputView& input
    );

    [[nodiscard]] RiskPipelineResult evaluate_view(
        const RiskInputView& input,
        IRiskDecisionPublisher* decision_publisher = nullptr,
        bool enable_full_audit_trace = false
    );

    [[nodiscard]] RiskPipelineResult evaluate_decision(
        const signal::OpportunityIntent& intent,
        const order_decision::OrderDecisionLite& decision,
        const RiskInputView& input,
        IRiskDecisionPublisher* decision_publisher = nullptr,
        bool enable_full_audit_trace = false
    );

    [[nodiscard]] RiskPipelineResult evaluate_decision(
        const signal::OpportunityIntent& intent,
        const order_decision::OrderDecision& decision,
        const RiskInputView& input,
        IRiskDecisionPublisher* decision_publisher = nullptr,
        bool enable_full_audit_trace = false
    );

    [[nodiscard]] RiskPipelineResult evaluate_decision(
        const order_decision::OrderDecisionLite& decision,
        const RiskInputView& input,
        IRiskDecisionPublisher* decision_publisher = nullptr,
        bool enable_full_audit_trace = false
    );

    [[nodiscard]] RiskPipelineResult evaluate_decision(
        const order_decision::OrderDecision& decision,
        const RiskInputView& input,
        IRiskDecisionPublisher* decision_publisher = nullptr,
        bool enable_full_audit_trace = false
    );

    void set_runtime_context(RiskRuntimeContext runtime_context);

    [[nodiscard]] RiskLedgerSnapshot ledger_snapshot() const;

    void release_reservation(std::uint64_t reservation_id);
    void expire_old(std::uint64_t now_ns);

private:
    [[nodiscard]] ReservationBook* reservation_book() noexcept;
    [[nodiscard]] const ReservationBook* reservation_book() const noexcept;

    ReservationBook reservations_;
    RiskPipeline pipeline_;
    RiskRuntimeContext runtime_;
    RiskLedgerSnapshot ledger_snapshot_cache_;
};

}  // namespace trading_engine::risk
