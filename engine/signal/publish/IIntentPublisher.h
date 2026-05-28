#pragma once

#include "engine/signal/public/OpportunityIntent.h"

namespace trading_engine::signal {

class IIntentPublisher {
public:
    virtual void publish(const OpportunityIntent& intent) = 0;
    virtual ~IIntentPublisher() = default;
};

}  // namespace trading_engine::signal
