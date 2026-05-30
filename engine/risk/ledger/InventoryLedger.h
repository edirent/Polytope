#pragma once

#include "engine/risk/ledger/Reservation.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace trading_engine::risk {

struct InventorySnapshot {
    std::unordered_map<std::string, std::int64_t> asset_reserved_lots;
};

class InventoryLedger {
public:
    void reserve(const RiskReservation& reservation);
    void release(const RiskReservation& reservation);

    [[nodiscard]] InventorySnapshot snapshot() const;

private:
    std::unordered_map<std::string, std::int64_t> asset_reserved_lots_;
};

}  // namespace trading_engine::risk
