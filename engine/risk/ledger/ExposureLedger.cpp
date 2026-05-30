#include "engine/risk/ledger/ExposureLedger.h"

namespace trading_engine::risk {

void ExposureLedger::reserve(const RiskReservation& reservation) {
    total_reserved_cost_tick_ += reservation.reserved_cost_tick;
    total_reserved_exposure_tick_ += reservation.reserved_exposure_tick;

    for (const auto& [market_id, exposure] :
         reservation.reserved_market_exposure_tick) {
        market_reserved_exposure_tick_[market_id] += exposure;
    }
}

void ExposureLedger::release(const RiskReservation& reservation) {
    total_reserved_cost_tick_ -= reservation.reserved_cost_tick;
    total_reserved_exposure_tick_ -= reservation.reserved_exposure_tick;

    for (const auto& [market_id, exposure] :
         reservation.reserved_market_exposure_tick) {
        auto it = market_reserved_exposure_tick_.find(market_id);
        if (it == market_reserved_exposure_tick_.end()) {
            continue;
        }
        it->second -= exposure;
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
