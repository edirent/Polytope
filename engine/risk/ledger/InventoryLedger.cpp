#include "engine/risk/ledger/InventoryLedger.h"

namespace trading_engine::risk {

void InventoryLedger::reserve(const RiskReservation& reservation) {
    for (const auto& [asset_id, lots] : reservation.reserved_asset_lots) {
        asset_reserved_lots_[asset_id] += lots;
    }
}

void InventoryLedger::release(const RiskReservation& reservation) {
    for (const auto& [asset_id, lots] : reservation.reserved_asset_lots) {
        auto it = asset_reserved_lots_.find(asset_id);
        if (it == asset_reserved_lots_.end()) {
            continue;
        }
        it->second -= lots;
        if (it->second == 0) {
            asset_reserved_lots_.erase(it);
        }
    }
}

InventorySnapshot InventoryLedger::snapshot() const {
    InventorySnapshot out;
    out.asset_reserved_lots = asset_reserved_lots_;
    return out;
}

}  // namespace trading_engine::risk
