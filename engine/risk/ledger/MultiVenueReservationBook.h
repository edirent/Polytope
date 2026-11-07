#pragma once
#include "engine/risk/ledger/RiskLedger.h"
#include "engine/strategy/cross_venue/PairScanner.h"
#include <set>
#include <memory>

namespace trading_engine::cross_venue {
class PairExecutionStateMachine;
struct PairReservation {
    std::uint64_t id = 0;
    CrossVenueOpportunity opportunity;
    std::array<Money, 2> cash{};
};
// Portfolio ownership remains in the existing risk ledger. Multiple pair
// coordinators may share these two venue wallets during sequential replay.
struct SharedVenueCapital {
    std::array<Money,2> cash{};
    std::array<risk::RiskLedger,2> reservations;
};

// Extends the existing risk ledger with two separately funded accounts.
// Single-writer coordinator owns this object; no network or live order entry.
class MultiVenueReservationBook {
public:
    MultiVenueReservationBook(const ApprovedPair& pair, std::array<Money, 2> cash);
    MultiVenueReservationBook(const ApprovedPair& pair, std::shared_ptr<SharedVenueCapital> capital);
    PairReservation reserve(const CrossVenueOpportunity& opportunity, const ApprovedPair& pair,
        const SynchronizedBookStore& store, const FeeQuote& fees, const PairPolicy& policy, std::uint64_t now_ns);
    Money reserved_cash(std::size_t leg) const;
    Money available_cash(std::size_t leg) const;
    void release_reconciled(const PairExecutionStateMachine& execution, std::array<Money, 2> actual_spend);
    void retain_confirmed_spend(const PairExecutionStateMachine& execution);
private:
    ApprovedPair pair_;
    std::shared_ptr<SharedVenueCapital> capital_;
    std::array<std::size_t,2> wallets_{0,1};
    std::map<std::uint64_t, std::array<risk::RiskReservation, 2>> active_;
    std::set<std::string> used_candidates_;
    std::uint64_t next_id_ = 1;
};
}  // namespace trading_engine::cross_venue
