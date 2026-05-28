#include "engine/signal/publish/CapturingIntentPublisher.h"

namespace trading_engine::signal {

void CapturingIntentPublisher::publish(const OpportunityIntent& intent) {
    intents_.push_back(intent);
}

const std::vector<OpportunityIntent>& CapturingIntentPublisher::intents()
    const noexcept {
    return intents_;
}

}  // namespace trading_engine::signal
