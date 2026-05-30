#include "engine/risk/publish/ApprovedIntentPublisher.h"

namespace trading_engine::risk {

void CapturingApprovedIntentPublisher::publish_approved(
    const ApprovedIntent& intent
) {
    approved_.push_back(intent);
}

const std::vector<ApprovedIntent>&
CapturingApprovedIntentPublisher::approved_intents() const noexcept {
    return approved_;
}

}  // namespace trading_engine::risk
