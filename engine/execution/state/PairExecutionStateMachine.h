#pragma once
#include "engine/state/cross_venue/Types.h"
#include <set>

namespace trading_engine::cross_venue {
struct PairReservation;
enum class PairState {
    Detected, Reserved, Planned, LegASubmitted, LegAPartial, LegAFilled,
    LegBSubmitted, LegBPartial, LegBFilled, FullyHedged, SettlementPending,
    Reconciled, ResidualExposure, RehedgeOrStop, Freeze
};
std::string state_name(PairState state);

// SIMULATED event interpreter, never an order transport. Confirmed fills are
// delta quantities, not exchange cumulative-filled counters.
class PairExecutionStateMachine {
public:
    PairExecutionStateMachine(ApprovedPair pair, std::int64_t quantity);
    void reserve(const PairReservation& reservation);
    void plan();
    void submitted(std::size_t leg, const std::string& order_id);
    void acknowledged(std::size_t leg);
    void cancel_requested(std::size_t leg);
    void cancel_confirmed(std::size_t leg);
    bool confirmed_fill(const FillReceipt& receipt);
    void residual_stop();
    void settlement_pending();
    void reconciled(bool balances_match, bool settlement_receipts_match);
    void freeze(std::string reason);
    PairState state() const noexcept { return state_; }
    std::int64_t filled(std::size_t leg) const { return legs_.at(leg).filled; }
    bool all_orders_terminal() const;
    std::array<Money, 2> confirmed_spend() const;
    std::uint64_t reservation_id() const { return reservation_id_; }
    const std::string& candidate_id() const { return candidate_id_; }
    const std::string& pair_hash() const { return pair_.artifact_hash; }
    const std::vector<std::string>& trace() const { return trace_; }
private:
    struct Leg { std::string order; bool cancel_requested = false, cancelled = false; std::int64_t filled = 0; };
    ApprovedPair pair_;
    std::int64_t quantity_;
    std::array<Leg, 2> legs_;
    PairState state_ = PairState::Detected;
    std::uint64_t reservation_id_ = 0;
    std::string candidate_id_;
    std::map<std::string, FillReceipt> receipts_;
    std::vector<std::string> trace_{"DETECTED"};
    void transition(PairState state);
    void mutable_stage() const;
};
}  // namespace trading_engine::cross_venue
