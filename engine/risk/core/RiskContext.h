#pragma once

#include "engine/risk/ledger/RiskLedger.h"
#include "engine/risk/publish/RiskDecisionPublisher.h"
#include "engine/risk/public/ApprovedIntent.h"
#include "engine/risk/public/RiskAuditTrace.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/risk/public/RiskResult.h"
#include "engine/risk/reprice/CostRevalidationResult.h"
#include "engine/state/MarketStateSnapshot.h"

#include <cstdint>
#include <vector>

namespace trading_engine::risk {

class ReservationBook;

struct RiskRuntimeContext {
    const RiskPolicySnapshot* policy = nullptr;
    RiskLedger* ledger = nullptr;
    IRiskDecisionPublisher* publisher = nullptr;
    ReservationBook* reservation_book = nullptr;
    bool enable_full_audit_trace = false;
};

struct RiskEvaluationContext {
    std::uint64_t now_ns = 0;

    std::vector<state::MarketStateSnapshot> latest_snapshots;
    std::uint64_t latest_snapshot_version_hash = 0;

    RiskPolicySnapshot policy;

    RiskLedgerSnapshot ledger_snapshot;

    bool enable_full_audit_trace = false;

    IRiskDecisionPublisher* decision_publisher = nullptr;
};

struct RiskPipelineResult {
    RiskDecision decision;
    ApprovedIntent approved_intent;
    RiskResult result;
    CostRevalidationResult cost;
    ReservationResult reservation;
    RiskAuditTrace audit_trace;

    bool cost_revalidated = false;
    bool reservation_attempted = false;

    std::uint64_t output_hash = 0;
};

}  // namespace trading_engine::risk
