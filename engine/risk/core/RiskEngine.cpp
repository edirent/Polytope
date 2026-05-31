#include "engine/risk/core/RiskEngine.h"

namespace trading_engine::risk {

RiskEngine::RiskEngine() = default;

RiskEngine::RiskEngine(RiskRuntimeContext runtime_context)
    : runtime_(runtime_context) {}

RiskPipelineResult RiskEngine::evaluate(
    const signal::OpportunityIntent& intent,
    const RiskEvaluationContext& context
) {
    auto* reservations = reservation_book();
    reservations->expire_old(context.now_ns);
    ledger_snapshot_cache_ = runtime_.ledger != nullptr
        ? runtime_.ledger->snapshot()
        : reservations->snapshot();

    RiskInputView view;
    view.intent = &intent;
    view.snapshots = context.latest_snapshots.data();
    view.snapshot_count = static_cast<std::uint16_t>(
        context.latest_snapshots.size()
    );
    view.snapshot_version_hash = context.latest_snapshot_version_hash;
    view.now_ns = context.now_ns;
    view.policy = &context.policy;
    view.ledger = &ledger_snapshot_cache_;

    return pipeline_.evaluate_view(
        view,
        reservations,
        context.decision_publisher != nullptr
            ? context.decision_publisher
            : runtime_.publisher,
        context.enable_full_audit_trace || runtime_.enable_full_audit_trace
    );
}

RiskPipelineResult RiskEngine::evaluate(const RiskInputView& input) {
    return evaluate_view(input, nullptr, false);
}

RiskPipelineResult RiskEngine::evaluate_view(
    const RiskInputView& input,
    IRiskDecisionPublisher* decision_publisher,
    bool enable_full_audit_trace
) {
    auto* reservations = reservation_book();
    reservations->expire_old(input.now_ns);
    ledger_snapshot_cache_ = runtime_.ledger != nullptr
        ? runtime_.ledger->snapshot()
        : reservations->snapshot();

    auto view = input;
    view.ledger = &ledger_snapshot_cache_;
    if (view.policy == nullptr) {
        view.policy = runtime_.policy;
    }

    return pipeline_.evaluate_view(
        view,
        reservations,
        decision_publisher != nullptr ? decision_publisher : runtime_.publisher,
        enable_full_audit_trace || runtime_.enable_full_audit_trace
    );
}

void RiskEngine::set_runtime_context(RiskRuntimeContext runtime_context) {
    runtime_ = runtime_context;
}

RiskLedgerSnapshot RiskEngine::ledger_snapshot() const {
    return reservation_book()->snapshot();
}

void RiskEngine::release_reservation(std::uint64_t reservation_id) {
    reservation_book()->release(reservation_id);
}

void RiskEngine::expire_old(std::uint64_t now_ns) {
    reservation_book()->expire_old(now_ns);
}

ReservationBook* RiskEngine::reservation_book() noexcept {
    return runtime_.reservation_book != nullptr
        ? runtime_.reservation_book
        : &reservations_;
}

const ReservationBook* RiskEngine::reservation_book() const noexcept {
    return runtime_.reservation_book != nullptr
        ? runtime_.reservation_book
        : &reservations_;
}

}  // namespace trading_engine::risk
