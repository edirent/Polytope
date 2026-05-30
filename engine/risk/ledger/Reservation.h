#pragma once

#include "engine/risk/public/RiskTypes.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace trading_engine::risk {

enum class ReservationStatus : std::uint8_t {
    Active,
    Released,
    Expired,
    Consumed
};

struct RiskReservation {
    std::uint64_t reservation_id = 0;
    std::uint64_t intent_id = 0;
    std::uint64_t bundle_id = 0;

    std::int64_t reserved_cost_tick = 0;
    std::int64_t reserved_exposure_tick = 0;

    std::unordered_map<std::string, std::int64_t> reserved_asset_lots;
    std::unordered_map<std::string, std::int64_t>
        reserved_market_exposure_tick;

    std::uint64_t created_ts_ns = 0;
    std::uint64_t expires_at_ns = 0;

    ReservationStatus status = ReservationStatus::Active;

    std::string idempotency_key;
};

struct ReservationResult {
    bool ok = false;
    RiskRejectReason reject_reason = RiskRejectReason::NotEvaluated;
    std::string detail;
    std::uint64_t reservation_id = 0;
};

}  // namespace trading_engine::risk
