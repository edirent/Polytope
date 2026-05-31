#include "engine/risk/ledger/InventoryLedger.h"

namespace trading_engine::risk {

void InventoryLedger::reserve(const RiskReservation& reservation) {
    for (std::uint8_t i = 0; i < reservation.asset_count; ++i) {
        const auto& asset = reservation.assets[i];
        if (asset.asset_id == nullptr) {
            continue;
        }
        asset_reserved_lots_[*asset.asset_id] += asset.lots;
    }
}

void InventoryLedger::release(const RiskReservation& reservation) {
    for (std::uint8_t i = 0; i < reservation.asset_count; ++i) {
        const auto& asset = reservation.assets[i];
        if (asset.asset_id == nullptr) {
            continue;
        }
        auto it = asset_reserved_lots_.find(*asset.asset_id);
        if (it == asset_reserved_lots_.end()) {
            continue;
        }
        it->second -= asset.lots;
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
