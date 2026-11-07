#include "engine/execution/state/PairExecutionStateMachine.h"
#include "engine/risk/ledger/MultiVenueReservationBook.h"

namespace trading_engine::cross_venue {
std::string state_name(PairState s) {
    switch (s) {
        case PairState::Detected: return "DETECTED";
        case PairState::Reserved: return "RESERVED";
        case PairState::Planned: return "PLANNED";
        case PairState::LegASubmitted: return "LEG_A_SUBMITTED";
        case PairState::LegAPartial: return "LEG_A_PARTIAL";
        case PairState::LegAFilled: return "LEG_A_FILLED";
        case PairState::LegBSubmitted: return "LEG_B_SUBMITTED";
        case PairState::LegBPartial: return "LEG_B_PARTIAL";
        case PairState::LegBFilled: return "LEG_B_FILLED";
        case PairState::FullyHedged: return "FULLY_HEDGED";
        case PairState::SettlementPending: return "SETTLEMENT_PENDING";
        case PairState::Reconciled: return "RECONCILED";
        case PairState::ResidualExposure: return "RESIDUAL_EXPOSURE";
        case PairState::RehedgeOrStop: return "REHEDGE_OR_STOP";
        case PairState::Freeze: return "FREEZE";
    }
    throw std::invalid_argument("invalid state");
}
PairExecutionStateMachine::PairExecutionStateMachine(ApprovedPair p, std::int64_t q) : pair_(std::move(p)), quantity_(q) {
    validate_pair(pair_); require(q > 0 && q <= max_quantity, "invalid simulation quantity");
}
void PairExecutionStateMachine::transition(PairState s) { state_ = s; trace_.push_back(state_name(s)); }
void PairExecutionStateMachine::mutable_stage() const {
    require(state_ != PairState::Freeze && state_ != PairState::Reconciled && state_ != PairState::SettlementPending, "lifecycle is frozen/terminal");
}
void PairExecutionStateMachine::reserve(const PairReservation& r) {
    require(state_ == PairState::Detected && r.id > 0 && r.opportunity.artifact_hash == pair_.artifact_hash &&
        r.opportunity.approval_revision == pair_.approval_revision && r.opportunity.intent.bundle_qty == quantity_ &&
        !r.opportunity.candidate_id.empty(), "reserve out of order or wrong approval");
    reservation_id_ = r.id; candidate_id_ = r.opportunity.candidate_id; transition(PairState::Reserved);
}
void PairExecutionStateMachine::plan() { require(state_ == PairState::Reserved, "plan without reservation"); transition(PairState::Planned); }
void PairExecutionStateMachine::submitted(std::size_t i, const std::string& order) {
    mutable_stage(); require(state_ != PairState::Detected && state_ != PairState::Reserved && state_ != PairState::RehedgeOrStop &&
        !order.empty() && legs_.at(i).order.empty(), "invalid simulated submission");
    legs_[i].order = order; transition(i == 0 ? PairState::LegASubmitted : PairState::LegBSubmitted);
}
void PairExecutionStateMachine::acknowledged(std::size_t i) {
    mutable_stage(); require(!legs_.at(i).order.empty(), "ACK before submission"); trace_.push_back(i == 0 ? "LEG_A_ACK" : "LEG_B_ACK");
}
void PairExecutionStateMachine::cancel_requested(std::size_t i) {
    mutable_stage(); require(!legs_.at(i).order.empty(), "cancel before submission"); legs_[i].cancel_requested = true;
    trace_.push_back(i == 0 ? "LEG_A_CANCEL_REQUEST" : "LEG_B_CANCEL_REQUEST");
}
void PairExecutionStateMachine::cancel_confirmed(std::size_t i) {
    mutable_stage(); require(legs_.at(i).cancel_requested, "cancel confirmation without request"); legs_[i].cancelled = true;
    trace_.push_back(i == 0 ? "LEG_A_CANCEL_CONFIRMED" : "LEG_B_CANCEL_CONFIRMED");
    if (legs_[0].filled != legs_[1].filled) transition(PairState::ResidualExposure);
}
bool PairExecutionStateMachine::confirmed_fill(const FillReceipt& f) {
    validate_receipt(pair_, f);
    const auto it = receipts_.find(f.identity());
    if (it != receipts_.end()) { require(it->second == f, "conflicting duplicate fill"); return false; }
    auto& leg = legs_[f.canonical_yes ? 0 : 1];
    require(state_ != PairState::Detected && state_ != PairState::Reserved && state_ != PairState::Reconciled && state_ != PairState::SettlementPending,
            "fill outside planned lifecycle");
    require(!leg.order.empty() && leg.order == f.order_id && add(leg.filled, f.qty) <= quantity_, "unknown order/overfill");
    // Late confirmed fills remain inventory even after FREEZE/cancel ACK.
    leg.filled += f.qty; receipts_.emplace(f.identity(), f);
    trace_.push_back("CONFIRMED_FILL:" + f.identity());
    if (state_ != PairState::Freeze) {
        if (legs_[0].filled == quantity_ && legs_[1].filled == quantity_) transition(PairState::FullyHedged);
        else if (f.canonical_yes) transition(leg.filled == quantity_ ? PairState::LegAFilled : PairState::LegAPartial);
        else transition(leg.filled == quantity_ ? PairState::LegBFilled : PairState::LegBPartial);
        if (all_orders_terminal() && legs_[0].filled != legs_[1].filled) transition(PairState::ResidualExposure);
    }
    return true;
}
bool PairExecutionStateMachine::all_orders_terminal() const {
    for (const auto& leg : legs_) if (leg.order.empty() || (!leg.cancelled && leg.filled != quantity_)) return false;
    return true;
}
std::array<Money, 2> PairExecutionStateMachine::confirmed_spend() const {
    std::array<Money, 2> spend{};
    for (const auto& [id, f] : receipts_) {
        const auto i = f.canonical_yes ? 0U : 1U;
        spend[i] = add(spend[i], add(multiply(f.price, f.qty), f.fee_paid));
    }
    return spend;
}
void PairExecutionStateMachine::residual_stop() {
    mutable_stage(); require(all_orders_terminal() && legs_[0].filled != legs_[1].filled, "residual stop requires confirmed terminal orders");
    transition(PairState::ResidualExposure); transition(PairState::RehedgeOrStop);
}
void PairExecutionStateMachine::settlement_pending() {
    mutable_stage(); require(state_ == PairState::FullyHedged && all_orders_terminal(), "settlement before hedge complete"); transition(PairState::SettlementPending);
}
void PairExecutionStateMachine::reconciled(bool balances, bool settlements) {
    require(state_ == PairState::SettlementPending && balances && settlements && all_orders_terminal(), "unreconciled receipts/balances"); transition(PairState::Reconciled);
}
void PairExecutionStateMachine::freeze(std::string reason) {
    require(!reason.empty() && state_ != PairState::Reconciled, "invalid freeze"); transition(PairState::Freeze); trace_.push_back("REASON:" + reason);
}
}  // namespace trading_engine::cross_venue
