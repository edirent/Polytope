#pragma once

#include "engine/risk/public/RiskAuditTrace.h"

#include <iosfwd>

namespace trading_engine::risk {

class JsonlRiskDecisionWriter {
public:
    explicit JsonlRiskDecisionWriter(std::ostream* output);

    [[nodiscard]] bool write(
        const RiskDecision& decision,
        const RiskAuditTrace& trace
    );

private:
    std::ostream* output_ = nullptr;
};

}  // namespace trading_engine::risk
