#pragma once

#include "engine/signal/public/OpportunityIntent.h"
#include "engine/signal/public/SignalEvidenceView.h"

namespace trading_engine::signal {

class IIntentPublisher {
public:
    virtual void publish(const OpportunityIntent& intent) = 0;

    virtual void publish(
        const OpportunityIntent& intent,
        const SignalEvidenceView& evidence
    ) {
        static_cast<void>(evidence);
        publish(intent);
    }

    [[nodiscard]] virtual bool requires_materialized_strings() const noexcept {
        return true;
    }

    virtual ~IIntentPublisher() = default;
};

}  // namespace trading_engine::signal
