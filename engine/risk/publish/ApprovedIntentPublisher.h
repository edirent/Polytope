#pragma once

#include "engine/risk/public/ApprovedIntent.h"

#include <vector>

namespace trading_engine::risk {

class IApprovedIntentPublisher {
public:
    virtual ~IApprovedIntentPublisher() = default;

    virtual void publish_approved(const ApprovedIntent& intent) = 0;
};

class CapturingApprovedIntentPublisher final : public IApprovedIntentPublisher {
public:
    void publish_approved(const ApprovedIntent& intent) override;

    [[nodiscard]] const std::vector<ApprovedIntent>& approved_intents()
        const noexcept;

private:
    std::vector<ApprovedIntent> approved_;
};

}  // namespace trading_engine::risk
