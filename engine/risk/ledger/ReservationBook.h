#pragma once

#include "engine/risk/ledger/RiskLedger.h"
#include "engine/risk/public/RiskDecision.h"
#include "engine/signal/public/OpportunityIntent.h"

#include <cstdint>
#include <unordered_map>

namespace trading_engine::risk {

class ReservationBook {
public:
    ReservationResult try_reserve(
        const signal::OpportunityIntent& intent,
        const RiskDecision& decision,
        std::uint64_t now_ns
    );

    void release(std::uint64_t reservation_id);
    void expire_old(std::uint64_t now_ns);

    [[nodiscard]] RiskLedgerSnapshot snapshot() const;

private:
    [[nodiscard]] RiskReservation build_reservation(
        const signal::OpportunityIntent& intent,
        std::uint64_t now_ns
    ) const;

    std::uint64_t next_reservation_id_ = 1;
    std::unordered_map<std::uint64_t, RiskReservation> reservations_;
    std::unordered_map<std::string, std::uint64_t> active_idempotency_keys_;
    RiskLedger ledger_;
};

}  // namespace trading_engine::risk
