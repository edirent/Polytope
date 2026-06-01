#pragma once

#include <cstdint>

namespace trading_engine::paper {

enum class PaperMode : std::uint8_t {
    Disabled,
    Replay,
    Paper
};

enum class PaperEventType : std::uint8_t {
    OpportunityIntentObserved,
    RiskDecisionObserved,
    ApprovedIntentObserved,
    OrderPlanCreated,
    ExecutionReportObserved,
    ReservationDispositionObserved,
    MarkPriceUpdated,
    RegimeUpdated
};

struct PaperModuleStatus {
    std::uint32_t schema_version = 1;
    PaperMode mode = PaperMode::Disabled;
    bool network_enabled = false;
};

}  // namespace trading_engine::paper
