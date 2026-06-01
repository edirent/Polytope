#pragma once

#include "engine/decision_fastpath/core/FastPathGate.h"
#include "engine/decision_fastpath/kernel/FixedShapeKernelSpec.h"
#include "engine/execution/public/OrderPlan.h"
#include "engine/order_decision/public/OrderDecision.h"
#include "engine/risk/ledger/RiskLedger.h"
#include "engine/risk/public/ApprovedIntent.h"
#include "engine/risk/public/RiskDecision.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/view/MarketDepthView.h"

#include <cstdint>
#include <span>
#include <string>

namespace trading_engine::decision_fastpath {

enum class FastPathMode : std::uint8_t {
    Disabled,
    ShadowCompare,
    VerifiedPaper,
    PaperAuthoritative
};

using EventLocalPipelineMode = FastPathMode;

struct FastPathConfig {
    FastPathMode mode = FastPathMode::Disabled;

    bool enable_fixed_buy_kernel = false;
    bool enable_simd = false;

    std::uint64_t max_mismatches_before_disable = 1;

    double sample_verify_rate = 0.01;

    bool require_kernel_spec_hash_match = true;
    bool require_artifact_hash_match = true;
    bool require_policy_hash_match = true;
};

struct EventLocalDecisionPipelineConfig {
    std::uint64_t expected_artifact_hash = 0;
    std::uint64_t expected_constraint_hash = 0;
    std::uint64_t expected_policy_hash = 0;

    std::uint64_t intent_ttl_ns = 1'000'000'000;

    std::int64_t fee_per_bundle_tick = 0;
    std::int64_t latency_buffer_per_bundle_tick = 0;
    std::int64_t slippage_buffer_per_bundle_tick = 0;

    std::uint16_t max_supported_legs = kMaxFixedShapeKernelLegs;

    FastPathConfig fast_path;

    // Compatibility fields for older tests/tools. New callers should prefer
    // fast_path above.
    EventLocalPipelineMode mode = EventLocalPipelineMode::Disabled;

    bool fast_path_enabled = false;
    bool periodic_sample_verification_enabled = true;
    std::uint32_t sample_verification_interval = 1024;
};

struct EventLocalInput {
    std::uint64_t now_ns = 0;

    std::uint32_t dirty_asset_index = 0;

    const trading_engine::state::MarketDepthView* depth_views = nullptr;
    std::uint16_t depth_view_count = 0;

    const trading_engine::risk::RiskPolicySnapshot* policy = nullptr;
    const trading_engine::risk::RiskLedgerSnapshot* ledger = nullptr;

    std::uint64_t current_true_mask = 0;
    std::uint64_t current_false_mask = 0;
};

struct FastPathResult {
    bool produced_plan = false;
    bool fallback_required = false;

    EventLocalPipelineMode mode = EventLocalPipelineMode::Disabled;

    bool publish_allowed = false;
    bool reservation_allowed = false;
    bool generic_verification_required = true;
    bool authoritative = false;
    bool sample_verification_required = false;

    bool generic_comparison_performed = false;
    bool mismatch = false;
    std::uint64_t fast_opportunity_hash = 0;
    std::uint64_t fast_risk_hash = 0;
    std::uint64_t fast_plan_hash = 0;
    std::uint64_t fast_combined_hash = 0;
    std::uint64_t generic_opportunity_hash = 0;
    std::uint64_t generic_risk_hash = 0;
    std::uint64_t generic_plan_hash = 0;
    std::uint64_t generic_combined_hash = 0;
    std::uint64_t generic_output_hash = 0;
    std::string mismatch_reason;

    trading_engine::signal::OpportunityIntent intent;
    trading_engine::order_decision::OrderDecisionLite order_decision;
    trading_engine::risk::RiskDecision decision;
    trading_engine::risk::ApprovedIntent approved;
    trading_engine::execution::OrderPlan plan;

    std::uint64_t output_hash = 0;

    FastPathRejectReason reject_reason = FastPathRejectReason::None;
};

struct FastPathRuntimeState {
    bool disabled = false;
    std::uint64_t mismatch_count = 0;
    std::uint64_t last_fast_hash = 0;
    std::uint64_t last_generic_hash = 0;
    std::string last_mismatch_reason;
};

class EventLocalDecisionPipeline {
public:
    EventLocalDecisionPipeline(
        const FixedShapeKernelRegistry* registry,
        EventLocalDecisionPipelineConfig config = {}
    );

    [[nodiscard]] FastPathResult process(const EventLocalInput& input) const;

    [[nodiscard]] const FastPathRuntimeState& runtime_state() const noexcept;
    void reset_runtime_state() const noexcept;

private:
    const FixedShapeKernelRegistry* registry_ = nullptr;
    EventLocalDecisionPipelineConfig config_;
    mutable FastPathRuntimeState runtime_state_;
};

}  // namespace trading_engine::decision_fastpath
