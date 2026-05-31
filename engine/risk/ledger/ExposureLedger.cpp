#include "engine/risk/ledger/ExposureLedger.h"

namespace trading_engine::risk {

void ExposureLedger::reserve(const RiskReservation& reservation) {
    total_reserved_cost_tick_ += reservation.reserved_cost_tick;
    total_reserved_exposure_tick_ += reservation.reserved_exposure_tick;

    for (std::uint8_t i = 0; i < reservation.market_count; ++i) {
        const auto& market = reservation.markets[i];
        if (market.market_id == nullptr) {
            continue;
        }
        market_reserved_exposure_tick_[*market.market_id] +=
            market.exposure_tick;
    }
}

void ExposureLedger::release(const RiskReservation& reservation) {
    total_reserved_cost_tick_ -= reservation.reserved_cost_tick;
    total_reserved_exposure_tick_ -= reservation.reserved_exposure_tick;

    for (std::uint8_t i = 0; i < reservation.market_count; ++i) {
        const auto& market = reservation.markets[i];
        if (market.market_id == nullptr) {
            continue;
        }
        auto it = market_reserved_exposure_tick_.find(*market.market_id);
        if (it == market_reserved_exposure_tick_.end()) {
            continue;
        }
        it->second -= market.exposure_tick;
        if (it->second == 0) {
            market_reserved_exposure_tick_.erase(it);
        }
    }
}

ExposureSnapshot ExposureLedger::snapshot() const {
    ExposureSnapshot out;
    out.total_reserved_cost_tick = total_reserved_cost_tick_;
    out.total_reserved_exposure_tick = total_reserved_exposure_tick_;
    out.market_reserved_exposure_tick = market_reserved_exposure_tick_;
    return out;
}

}  // namespace trading_engine::risk
