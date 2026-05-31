#pragma once

#include "engine/risk/ledger/RiskLedger.h"
#include "engine/risk/public/RiskDecision.h"
#include "engine/signal/public/OpportunityIntent.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace trading_engine::risk {

struct ReservationKey {
    std::uint64_t idempotency_hash = 0;
    std::uint64_t bundle_id = 0;

    [[nodiscard]] bool operator==(const ReservationKey& other) const noexcept {
        return idempotency_hash == other.idempotency_hash &&
               bundle_id == other.bundle_id;
    }
};

struct ReservationKeyHash {
    [[nodiscard]] std::size_t operator()(
        const ReservationKey& key
    ) const noexcept {
        auto hash = std::hash<std::uint64_t>{}(key.idempotency_hash);
        hash ^= std::hash<std::uint64_t>{}(key.bundle_id) +
                0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
        return hash;
    }
};

class ReservationBook {
public:
    ReservationBook();

    ReservationResult try_reserve(
        const signal::OpportunityIntent& intent,
        const RiskDecision& decision,
        std::uint64_t now_ns,
        bool track_ledger_detail = true
    );

    void release(std::uint64_t reservation_id);
    void expire_old(std::uint64_t now_ns);

    [[nodiscard]] RiskLedgerSnapshot snapshot() const;

private:
    void build_reservation(
        const signal::OpportunityIntent& intent,
        std::uint64_t now_ns,
        RiskReservation* reservation,
        bool track_ledger_detail
    );

    [[nodiscard]] const std::string* intern_string(const std::string& value);

    std::uint64_t next_reservation_id_ = 1;
    std::vector<RiskReservation> reservations_;
    std::unordered_map<ReservationKey, std::uint64_t, ReservationKeyHash>
        active_reservation_keys_;
    std::unordered_set<std::string> interned_strings_;
    RiskLedger ledger_;
};

}  // namespace trading_engine::risk
