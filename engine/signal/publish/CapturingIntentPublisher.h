#pragma once

#include "engine/signal/publish/IIntentPublisher.h"

#include <vector>

namespace trading_engine::signal {

class CapturingIntentPublisher final : public IIntentPublisher {
public:
    void publish(const OpportunityIntent& intent) override;

    [[nodiscard]] const std::vector<OpportunityIntent>& intents() const noexcept;

private:
    std::vector<OpportunityIntent> intents_;
};

}  // namespace trading_engine::signal
