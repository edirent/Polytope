#pragma once

#include "engine/signal/public/OpportunityIntent.h"

namespace trading_engine::signal {

class OpportunityFilter {
public:
    [[nodiscard]] bool should_emit(
        const OpportunityIntent&
    ) const noexcept {
        return true;
    }
};

}  // namespace trading_engine::signal
