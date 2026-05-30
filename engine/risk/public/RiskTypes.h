#pragma once

#include <cstdint>

namespace trading_engine::risk {

enum class RiskDecisionStatus : std::uint8_t {
    Rejected,
    Approved
};

enum class RiskDecisionType : std::uint8_t {
    Pass,
    Approve = Pass,
    RejectKillSwitch,
    RejectExpiredIntent,
    RejectDuplicateIntent,
    RejectRateLimited,
    RejectBadMarketState,
    RejectStaleSnapshot,
    RejectInsufficientDepth,
    RejectCostDrift,
    RejectReducedBundleQty,
    RejectLowTotalEdge,
    RejectLowUnitEdge,
    RejectLowEdgeBps,
    RejectCostLimit,
    RejectTotalExposureLimit,
    RejectSingleMarketExposureLimit,
    RejectInventoryLimit,
    RejectPartialFillRisk,
    RejectInternalError
};

enum class RiskRejectReason : std::uint8_t {
    None,
    NotEvaluated,
    RiskDisabled,
    KillSwitch,
    LowTotalEdge,
    LowUnitEdge,
    LowEdgeBps,
    CostLimit,
    SingleMarketExposureLimit,
    TotalExposureLimit,
    InventoryLimit,
    BadMarketState,
    StaleBook,
    ExpiredIntent,
    SnapshotSkew,
    CostDrift,
    SlippageLimit,
    PendingIntentLimit,
    ApprovalRateLimit,
    MissingReservation,
    InvalidIntent,
    MissingEvidence,
    DuplicateIntent,
    DuplicateReservation,
    PartialFillRisk,
    InternalError
};

}  // namespace trading_engine::risk
