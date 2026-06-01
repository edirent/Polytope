#pragma once

#include "engine/risk/ledger/ExposureLedger.h"
#include "engine/risk/ledger/InventoryLedger.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace trading_engine::risk {

struct NumericReservedAssetLot {
    std::uint32_t asset_index = 0;
    std::int64_t lots = 0;
};

struct NumericReservedMarketExposure {
    std::uint32_t market_index = 0;
    std::int64_t exposure_tick = 0;
};

struct RiskLedgerSnapshot {
    std::uint64_t active_reservations = 0;
    std::uint64_t released_reservations = 0;
    std::uint64_t expired_reservations = 0;
    std::uint64_t consumed_reservations = 0;

    std::int64_t total_reserved_cost_tick = 0;
    std::int64_t total_reserved_exposure_tick = 0;

    std::unordered_map<std::string, std::int64_t> reserved_asset_lots;
    std::unordered_map<std::string, std::int64_t>
        reserved_market_exposure_tick;

    std::uint8_t numeric_asset_count = 0;
    std::array<NumericReservedAssetLot, kMaxReservedLegs>
        numeric_reserved_asset_lots{};

    std::uint8_t numeric_market_count = 0;
    std::array<NumericReservedMarketExposure, kMaxReservedLegs>
        numeric_reserved_market_exposure{};
};

class RiskLedger {
public:
    void reserve(const RiskReservation& reservation);
    void release(const RiskReservation& reservation);

    [[nodiscard]] RiskLedgerSnapshot snapshot() const;

private:
    ExposureLedger exposure_;
    InventoryLedger inventory_;
};

}  // namespace trading_engine::risk
