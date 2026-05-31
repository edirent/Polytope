#pragma once

#include "engine/risk/public/RiskTypes.h"

#include <array>
#include <cstdint>
#include <string>

namespace trading_engine::risk {

inline constexpr std::uint8_t kMaxReservedLegs = 16;

enum class ReservationStatus : std::uint8_t {
    Active,
    Released,
    Expired,
    Consumed
};

struct ReservedAssetLot {
    std::uint32_t asset_index = 0;
    const std::string* asset_id = nullptr;
    std::int64_t lots = 0;
};

struct ReservedMarketExposure {
    std::uint32_t market_index = 0;
    const std::string* market_id = nullptr;
    std::int64_t exposure_tick = 0;
};

struct RiskReservation {
    std::uint64_t reservation_id = 0;
    std::uint64_t intent_id = 0;
    std::uint64_t bundle_id = 0;
    std::uint64_t idempotency_hash = 0;

    std::int64_t reserved_cost_tick = 0;
    std::int64_t reserved_exposure_tick = 0;

    std::uint8_t asset_count = 0;
    std::array<ReservedAssetLot, kMaxReservedLegs> assets{};

    std::uint8_t market_count = 0;
    std::array<ReservedMarketExposure, kMaxReservedLegs> markets{};

    std::uint64_t created_ts_ns = 0;
    std::uint64_t expires_at_ns = 0;

    ReservationStatus status = ReservationStatus::Active;
    bool ledger_reserved = false;

    std::string idempotency_key;
};

struct ReservationResult {
    bool ok = false;
    RiskRejectReason reject_reason = RiskRejectReason::NotEvaluated;
    std::string detail;
    std::uint64_t reservation_id = 0;
};

}  // namespace trading_engine::risk
