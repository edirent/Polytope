#pragma once

#include "engine/risk/core/RiskContext.h"
#include "engine/risk/core/RiskScratch.h"
#include "engine/risk/guards/DuplicateIntentGuard.h"
#include "engine/risk/guards/EdgeGuard.h"
#include "engine/risk/guards/ExposureGuard.h"
#include "engine/risk/guards/IntentExpiryGuard.h"
#include "engine/risk/guards/InventoryGuard.h"
#include "engine/risk/guards/KillSwitchGuard.h"
#include "engine/risk/guards/MarketStateGuard.h"
#include "engine/risk/guards/MaxLossGuard.h"
#include "engine/risk/guards/PartialFillGuard.h"
#include "engine/risk/guards/RateLimitGuard.h"
#include "engine/risk/guards/SnapshotFreshnessGuard.h"
#include "engine/risk/ledger/ReservationBook.h"
#include "engine/risk/public/RiskInputView.h"
#include "engine/risk/reprice/CostRevalidator.h"
#include "engine/risk/validate/IntentEvidenceVerifier.h"
#include "engine/risk/validate/IntentValidator.h"

namespace trading_engine::risk {

class RiskPipeline {
public:
    RiskPipeline();

    [[nodiscard]] RiskPipelineResult evaluate(
        const signal::OpportunityIntent& intent,
        const RiskEvaluationContext& context,
        ReservationBook* reservations
    );

    [[nodiscard]] RiskPipelineResult evaluate_view(
        const RiskInputView& input,
        ReservationBook* reservations,
        IRiskDecisionPublisher* decision_publisher = nullptr,
        bool enable_full_audit_trace = false
    );

    void clear_duplicate_cache();

private:
    IntentValidator intent_validator_;
    IntentEvidenceVerifier evidence_verifier_;
    IntentExpiryGuard expiry_guard_;
    DuplicateIntentGuard duplicate_guard_;
    RateLimitGuard rate_limit_guard_;
    MarketStateGuard market_state_guard_;
    SnapshotFreshnessGuard snapshot_freshness_guard_;
    CostRevalidator cost_revalidator_;
    EdgeGuard edge_guard_;
    MaxLossGuard max_loss_guard_;
    ExposureGuard exposure_guard_;
    InventoryGuard inventory_guard_;
    PartialFillGuard partial_fill_guard_;
    RiskScratch scratch_;
};

}  // namespace trading_engine::risk
