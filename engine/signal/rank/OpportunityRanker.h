#pragma once

#include "engine/signal/public/OpportunityIntent.h"

#include <vector>

namespace trading_engine::signal {

class OpportunityRanker {
public:
    void rank(std::vector<OpportunityIntent>* intents) const;
};

}  // namespace trading_engine::signal
