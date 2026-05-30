#pragma once

#include "engine/risk/ledger/Reservation.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace trading_engine::risk {

struct ExposureSnapshot {
    std::int64_t total_reserved_cost_tick = 0;
    std::int64_t total_reserved_exposure_tick = 0;
    std::unordered_map<std::string, std::int64_t>
        market_reserved_exposure_tick;
};

class ExposureLedger {
public:
    void reserve(const RiskReservation& reservation);
    void release(const RiskReservation& reservation);

    [[nodiscard]] ExposureSnapshot snapshot() const;

private:
    std::int64_t total_reserved_cost_tick_ = 0;
    std::int64_t total_reserved_exposure_tick_ = 0;
    std::unordered_map<std::string, std::int64_t>
        market_reserved_exposure_tick_;
};

}  // namespace trading_engine::risk
