#include "engine/risk/ledger/MultiVenueReservationBook.h"
#include "engine/execution/state/PairExecutionStateMachine.h"

namespace trading_engine::cross_venue {
MultiVenueReservationBook::MultiVenueReservationBook(const ApprovedPair& p, std::array<Money, 2> cash) : pair_(p),capital_(std::make_shared<SharedVenueCapital>()) {
    validate_pair(p);
    require(cash[0] >= 0 && cash[1] >= 0, "negative starting cash");
    capital_->cash=cash;
}
MultiVenueReservationBook::MultiVenueReservationBook(const ApprovedPair& p,std::shared_ptr<SharedVenueCapital> capital):pair_(p),capital_(std::move(capital)) {
    validate_pair(p);require(capital_ && capital_->cash[0]>=0 && capital_->cash[1]>=0,"invalid shared capital");
    wallets_={p.leg_yes.venue==VenueId::Kalshi?0U:1U,p.leg_no.venue==VenueId::Kalshi?0U:1U};
    require(wallets_[0]!=wallets_[1],"unsupported portfolio venue");
}
Money MultiVenueReservationBook::reserved_cash(std::size_t i) const { return capital_->reservations.at(wallets_.at(i)).snapshot().total_reserved_cost_tick; }
Money MultiVenueReservationBook::available_cash(std::size_t i) const { return add(capital_->cash.at(wallets_.at(i)), -reserved_cash(i)); }
PairReservation MultiVenueReservationBook::reserve(const CrossVenueOpportunity& o, const ApprovedPair& p,
    const SynchronizedBookStore& store, const FeeQuote& fee, const PairPolicy& policy, std::uint64_t now) {
    require(p.artifact_hash == pair_.artifact_hash, "reservation account/pair revision mismatch");
    const auto fresh = PairScanner::quote(p, store.read_pair(p), fee, policy, now);
    require(o.candidate_id == fresh.candidate_id && o.approval_revision == p.approval_revision && o.artifact_hash == p.artifact_hash &&
            o.costs == fresh.costs && o.fees == fresh.fees && o.slippage == fresh.slippage &&
            o.intent.bundle_qty == fresh.intent.bundle_qty, "intent evidence/revision changed");
    require(active_.empty(), "one active pair per V0 coordinator");
    require(!used_candidates_.contains(o.candidate_id), "duplicate candidate");
    require(multiply(fresh.intent.guaranteed_payout_tick, 2) <= policy.max_total_payout, "total notional limit");
    require(fresh.intent.total_edge_tick >= fresh.intent.min_edge_tick, "profit below risk threshold");
    PairReservation result{.id = next_id_, .opportunity = fresh};
    std::array<risk::RiskReservation, 2> reservations{};
    for (std::size_t i = 0; i < 2; ++i) {
        result.cash[i] = add(fresh.costs[i], add(fresh.fees[i], fresh.slippage[i]));
        require(result.cash[i] <= policy.max_single_leg_loss, "single-leg worst-loss limit");
        require(available_cash(i) >= result.cash[i], "insufficient venue-local buying power");
        auto& r = reservations[i];
        r.reservation_id = next_id_; r.intent_id = fresh.intent.intent_id;
        r.idempotency_key = fresh.candidate_id;
        r.bundle_id = fresh.intent.bundle_id; r.reserved_cost_tick = result.cash[i];
        r.reserved_exposure_tick = multiply(fresh.intent.bundle_qty, unit);
    }
    // All validation precedes either account mutation.
    active_.emplace(next_id_, reservations);
    for (std::size_t i = 0; i < 2; ++i) capital_->reservations[wallets_[i]].reserve(reservations[i]);
    used_candidates_.insert(o.candidate_id); ++next_id_;
    return result;
}
void MultiVenueReservationBook::release_reconciled(const PairExecutionStateMachine& e, std::array<Money, 2> spend) {
    require(e.state() == PairState::Reconciled && e.pair_hash() == pair_.artifact_hash, "release requires reconciled matching lifecycle");
    const auto it = active_.find(e.reservation_id());
    require(it != active_.end(), "unknown reservation");
    require(e.candidate_id() == it->second[0].idempotency_key, "lifecycle candidate does not match reservation");
    require(spend == e.confirmed_spend(), "actual spend must match confirmed receipts");
    for (std::size_t i = 0; i < 2; ++i) require(spend[i] >= 0 && spend[i] <= capital_->cash[wallets_[i]], "invalid actual spend");
    for (std::size_t i = 0; i < 2; ++i) { capital_->cash[wallets_[i]] -= spend[i]; capital_->reservations[wallets_[i]].release(it->second[i]); }
    active_.erase(it);
}
void MultiVenueReservationBook::retain_confirmed_spend(const PairExecutionStateMachine& e) {
    const auto it=active_.find(e.reservation_id());require(it!=active_.end() && e.pair_hash()==pair_.artifact_hash && e.candidate_id()==it->second[0].idempotency_key,"unknown confirmed-spend reservation");
    const auto spent=e.confirmed_spend();
    for(std::size_t i=0;i<2;++i)require(spent[i]<=add(available_cash(i),it->second[i].reserved_cost_tick),"confirmed spend exceeds portfolio cash");
    for(std::size_t i=0;i<2;++i)if(spent[i]>it->second[i].reserved_cost_tick){auto& ledger=capital_->reservations[wallets_[i]];
        ledger.release(it->second[i]);it->second[i].reserved_cost_tick=spent[i];ledger.reserve(it->second[i]);}
}
}  // namespace trading_engine::cross_venue
