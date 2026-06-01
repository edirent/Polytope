#pragma once

#include "engine/paper/public/PaperTypes.h"

#include <cstdint>

namespace trading_engine::paper {

struct PaperEvent {
    std::uint64_t seq_no = 0;
    std::uint64_t ts_ns = 0;
    PaperEventType type = PaperEventType::OpportunityIntentObserved;
};

}  // namespace trading_engine::paper
