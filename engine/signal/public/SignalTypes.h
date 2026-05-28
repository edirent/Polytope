#pragma once

#include "oracle/public/CandidateBundle.h"

#include <cstdint>

namespace trading_engine::signal {

using Side = trading_engine::oracle::Side;

enum class IntentStatus : std::uint8_t {
    CandidateOnly,

    RejectedInvalidSettlement,
    RejectedBadMarketState,
    RejectedMissingSnapshot,
    RejectedInsufficientDepth,
    RejectedLowEdge,

    PaperOpportunity
};

}  // namespace trading_engine::signal
