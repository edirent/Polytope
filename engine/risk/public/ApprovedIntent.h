#pragma once

#include "engine/risk/public/RiskDecision.h"
#include "engine/signal/public/OpportunityIntent.h"

#include <cstdint>
#include <string>

namespace trading_engine::risk {

struct ApprovedIntent {
    trading_engine::signal::OpportunityIntent intent;
    RiskDecision decision;

    std::string reservation_id;

    std::uint64_t approved_at_ns = 0;
    std::uint64_t expires_at_ns = 0;

    [[nodiscard]] bool has_reservation() const noexcept {
        return !reservation_id.empty();
    }

    [[nodiscard]] bool valid() const noexcept {
        return decision.approved() && has_reservation();
    }
};

}  // namespace trading_engine::risk
