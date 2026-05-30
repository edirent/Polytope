#include "engine/risk/ledger/RiskLedger.h"

namespace trading_engine::risk {

void RiskLedger::reserve(const RiskReservation& reservation) {
    exposure_.reserve(reservation);
    inventory_.reserve(reservation);
}

void RiskLedger::release(const RiskReservation& reservation) {
    exposure_.release(reservation);
    inventory_.release(reservation);
}

RiskLedgerSnapshot RiskLedger::snapshot() const {
    const auto exposure_snapshot = exposure_.snapshot();
    const auto inventory_snapshot = inventory_.snapshot();

    RiskLedgerSnapshot out;
    out.total_reserved_cost_tick =
        exposure_snapshot.total_reserved_cost_tick;
    out.total_reserved_exposure_tick =
        exposure_snapshot.total_reserved_exposure_tick;
    out.reserved_market_exposure_tick =
        exposure_snapshot.market_reserved_exposure_tick;
    out.reserved_asset_lots = inventory_snapshot.asset_reserved_lots;
    return out;
}

}  // namespace trading_engine::risk
