#include "engine/signal/core/SignalEngine.h"

#include "engine/signal/publish/IntentBuilder.h"
#include "engine/signal/publish/IntentId.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <span>
#include <utility>

namespace trading_engine::signal {

namespace {

[[nodiscard]] SignalPublishedEvidence evidence_from_batch(
    const SnapshotBatchReadResult& snapshots,
    std::uint64_t read_ts_ns
) {
    SignalPublishedEvidence evidence;
    evidence.snapshot_count = std::min<std::uint16_t>(
        snapshots.snapshot_count,
        kMaxIntentLegs
    );
    for (std::uint16_t i = 0; i < evidence.snapshot_count; ++i) {
        evidence.snapshots[i] = snapshots.snapshots[i];
    }
    evidence.snapshot_version_hash = snapshots.snapshot_version.combined_hash;
    evidence.read_ts_ns = read_ts_ns;
    return evidence;
}

[[nodiscard]] SignalPublishedEvidence evidence_from_depth(
    const DepthReadResult& depth,
    std::uint64_t read_ts_ns
) {
    SignalPublishedEvidence evidence;
    evidence.depth_view_count = std::min<std::uint16_t>(
        depth.count,
        kMaxIntentLegs
    );
    for (std::uint16_t i = 0; i < evidence.depth_view_count; ++i) {
        evidence.depth_views[i] = depth.depth_views[i];
    }
    evidence.snapshot_version_hash = depth.snapshot_version.combined_hash;
    evidence.read_ts_ns = read_ts_ns;
    return evidence;
}

[[nodiscard]] SignalPublishedEvidence evidence_from_snapshots(
    const SnapshotReadResult& snapshots,
    std::uint64_t read_ts_ns
) {
    SignalPublishedEvidence evidence;
    evidence.snapshot_count = std::min<std::uint16_t>(
        static_cast<std::uint16_t>(snapshots.snapshots.size()),
        kMaxIntentLegs
    );
    for (std::uint16_t i = 0; i < evidence.snapshot_count; ++i) {
        evidence.snapshots[i] = snapshots.snapshots[i];
    }
    evidence.snapshot_version_hash = snapshots.snapshot_version.combined_hash;
    evidence.read_ts_ns = read_ts_ns;
    return evidence;
}

[[nodiscard]] int status_priority(IntentStatus status) noexcept {
    return status == IntentStatus::PaperOpportunity ? 0 : 1;
}

[[nodiscard]] bool candidate_less(
    const SignalPublishCandidate& left,
    const SignalPublishCandidate& right
) noexcept {
    const auto left_status = status_priority(left.intent.status);
    const auto right_status = status_priority(right.intent.status);
    if (left_status != right_status) {
        return left_status < right_status;
    }
    if (left.intent.total_edge_tick != right.intent.total_edge_tick) {
        return left.intent.total_edge_tick > right.intent.total_edge_tick;
    }
    if (left.intent.edge_bps != right.intent.edge_bps) {
        return left.intent.edge_bps > right.intent.edge_bps;
    }
    if (left.intent.bundle_qty != right.intent.bundle_qty) {
        return left.intent.bundle_qty > right.intent.bundle_qty;
    }
    if (left.intent.bundle_id != right.intent.bundle_id) {
        return left.intent.bundle_id < right.intent.bundle_id;
    }
    return left.intent.intent_id < right.intent.intent_id;
}

[[nodiscard]] std::uint64_t make_intent_id(
    const SignalScanContext& context,
    std::uint64_t ordinal
) noexcept {
    return (context.scan_id << 32U) ^ ordinal;
}

void copy_bundle_legs(
    const CandidateBundle& bundle,
    OpportunityIntent* intent
) {
    intent->leg_count = std::min<std::uint16_t>(
        bundle.leg_count,
        kMaxIntentLegs
    );
    for (std::uint16_t i = 0; i < intent->leg_count; ++i) {
        const auto& source = bundle.legs[i];
        auto& target = intent->legs[i];
        target.market_id = source.market_id;
        target.asset_id = source.asset_id;
        target.side = source.side;
        target.quantity_lots = source.quantity_lots;
    }
}

void copy_plan_asset_indices(
    const BundleRuntimePlan& plan,
    OpportunityIntent* intent
) {
    if (!intent) {
        return;
    }
    for (std::uint16_t i = 0;
         i < intent->leg_count && i < plan.leg_count && i < kMaxIntentLegs;
         ++i) {
        intent->legs[i].asset_index = plan.asset_indices[i];
    }
}

[[nodiscard]] std::int64_t saturating_mul_i64(
    std::int64_t lhs,
    std::int64_t rhs
) noexcept {
    const auto value =
        static_cast<__int128>(lhs) * static_cast<__int128>(rhs);
    if (value > std::numeric_limits<std::int64_t>::max()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value < std::numeric_limits<std::int64_t>::min()) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] std::uint64_t saturating_add_u64(
    std::uint64_t lhs,
    std::uint64_t rhs
) noexcept {
    const auto value =
        static_cast<unsigned __int128>(lhs) +
        static_cast<unsigned __int128>(rhs);
    if (value > std::numeric_limits<std::uint64_t>::max()) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(value);
}

[[nodiscard]] std::int64_t depth_margin_bps(
    std::int64_t executable_qty_lots,
    std::int64_t requested_qty_lots
) noexcept {
    if (requested_qty_lots <= 0 || executable_qty_lots <= 0) {
        return 0;
    }
    const auto value =
        static_cast<__int128>(executable_qty_lots) * 10'000 /
        static_cast<__int128>(requested_qty_lots);
    if (value > std::numeric_limits<std::int64_t>::max()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] std::uint64_t proof_hash_for(
    const OpportunityIntent& intent
) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    auto mix = [&hash](std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
            hash *= 1099511628211ULL;
        }
    };

    mix(intent.oracle_artifact_hash);
    mix(intent.constraint_hash);
    mix(intent.bundle_hash);
    mix(intent.snapshot_version_hash);
    return hash;
}

void complete_intent_lifecycle(
    OpportunityIntent* intent,
    const SignalConfig& config,
    const OracleArtifactReader& artifact_reader,
    std::uint64_t now_ns
) {
    if (intent == nullptr) {
        return;
    }

    if (intent->created_ts_ns == 0) {
        intent->created_ts_ns = now_ns;
    }
    if (intent->expires_at_ns <= now_ns) {
        const auto ttl_ns = config.intent_ttl_ns == 0 ? 1 : config.intent_ttl_ns;
        intent->expires_at_ns = saturating_add_u64(now_ns, ttl_ns);
    }
    if (intent->oracle_artifact_version == 0) {
        intent->oracle_artifact_version = artifact_reader.artifact_version();
    }
    if (intent->oracle_artifact_hash == 0) {
        intent->oracle_artifact_hash = artifact_reader.artifact_hash();
    }
    if (intent->constraint_hash == 0) {
        intent->constraint_hash = artifact_reader.constraint_hash();
    }
    if (intent->bundle_hash == 0) {
        intent->bundle_hash = artifact_reader.bundle_hash();
    }
    if (intent->intent_id == 0) {
        const IntentIdentityInput identity{
            .bundle_id = intent->bundle_id,
            .bundle_hash = intent->bundle_hash,
            .snapshot_version_hash = intent->snapshot_version_hash,
            .bundle_qty = intent->bundle_qty,
            .unit_edge_tick = intent->unit_edge_tick
        };
        intent->intent_id = make_intent_id(identity);
    }
    if (intent->idempotency_hash == 0) {
        intent->idempotency_hash = intent->intent_id;
    }
    if (intent->proof_hash == 0) {
        intent->proof_hash = proof_hash_for(*intent);
    }
}

void copy_cost_fields(
    const CostResult& cost,
    OpportunityIntent* intent
) {
    intent->enough_depth = cost.executable;
    intent->bundle_qty = cost.bundle_qty;
    intent->original_bundle_qty = cost.bundle_qty;
    intent->estimated_cost_tick = cost.total_cost_tick;
    intent->slippage_buffer_tick = cost.max_leg_slippage_tick;
    intent->max_leg_slippage_tick = cost.max_leg_slippage_tick;
    intent->leg_count = std::max<std::uint16_t>(
        intent->leg_count,
        cost_leg_count(cost)
    );
    const auto priced_count = cost_leg_count(cost);
    for (std::uint16_t i = 0; i < intent->leg_count && i < priced_count; ++i) {
        const auto& source = cost_leg_at(cost, i);
        if (source.asset_id.empty()) {
            continue;
        }
        auto& target = intent->legs[i];
        target.asset_id = source.asset_id;
        target.quantity_lots = source.requested_qty_lots;
        target.estimated_vwap_tick = source.vwap_price_tick;
        target.worst_price_tick = source.worst_price_tick;
        target.estimated_cost_tick = source.total_cost_tick;
        target.requested_qty_lots = saturating_mul_i64(
            source.requested_qty_lots,
            cost.bundle_qty
        );
        target.executable_qty_lots = source.executable_qty_lots;
        target.depth_margin_bps = depth_margin_bps(
            target.executable_qty_lots,
            target.requested_qty_lots
        );
        target.enough_depth = source.enough_depth;
    }
}

[[nodiscard]] IntentStatus status_from_cost_failure(
    CostFailureReason reason
) noexcept {
    switch (reason) {
        case CostFailureReason::MissingSnapshot:
            return IntentStatus::RejectedMissingSnapshot;
        case CostFailureReason::MissingBookSide:
        case CostFailureReason::InsufficientDepth:
        case CostFailureReason::InvalidQuantity:
        case CostFailureReason::InvalidLeg:
            return IntentStatus::RejectedInsufficientDepth;
        case CostFailureReason::None:
            return IntentStatus::CandidateOnly;
    }
    return IntentStatus::RejectedInsufficientDepth;
}

[[nodiscard]] bool should_publish(
    const SignalConfig& config,
    IntentStatus status
) noexcept {
    return status == IntentStatus::PaperOpportunity || config.emit_rejections;
}

[[nodiscard]] bool is_stale_snapshot_rejection(
    const SnapshotReadResult& snapshots
) noexcept {
    return snapshots.rejection_status == IntentStatus::RejectedBadMarketState &&
           snapshots.error.find("stale") != std::string::npos;
}

[[nodiscard]] bool is_snapshot_skew_rejection(
    const SnapshotReadResult& snapshots
) noexcept {
    return snapshots.rejection_status == IntentStatus::RejectedBadMarketState &&
           snapshots.error.find("version") != std::string::npos;
}

[[nodiscard]] bool is_stale_snapshot_rejection(
    const SnapshotBatchReadResult& snapshots
) noexcept {
    return snapshots.rejection_status == IntentStatus::RejectedBadMarketState &&
           snapshots.error.find("stale") != std::string::npos;
}

[[nodiscard]] bool is_snapshot_skew_rejection(
    const SnapshotBatchReadResult& snapshots
) noexcept {
    return snapshots.rejection_status == IntentStatus::RejectedBadMarketState &&
           snapshots.error.find("version") != std::string::npos;
}

[[nodiscard]] bool is_stale_snapshot_rejection(
    const DepthReadResult& depth
) noexcept {
    return depth.rejection_status == IntentStatus::RejectedBadMarketState &&
           depth.error.find("stale") != std::string::npos;
}

[[nodiscard]] bool is_snapshot_skew_rejection(
    const DepthReadResult& depth
) noexcept {
    return depth.rejection_status == IntentStatus::RejectedBadMarketState &&
           depth.error.find("version") != std::string::npos;
}

[[nodiscard]] std::uint64_t elapsed_ns(
    std::chrono::steady_clock::time_point start
) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start
        ).count()
    );
}

void populate_metrics(
    SignalRunResult* result,
    std::uint64_t scan_latency_ns
) noexcept {
    if (!result) {
        return;
    }

    auto& metrics = result->metrics;
    metrics.scan_count = 1;
    metrics.bundle_scanned = result->bundles_scanned;
    metrics.bundle_passed = result->paper_opportunities;
    metrics.reject_settled = result->rejected_invalid_settlement;
    metrics.reject_missing_snapshot = result->rejected_missing_snapshot;
    metrics.reject_stale_lob = result->rejected_stale_snapshot;
    metrics.reject_snapshot_skew = result->rejected_snapshot_skew;
    metrics.reject_insufficient_depth = result->rejected_insufficient_depth;
    metrics.reject_edge_below_threshold = result->rejected_low_edge;
    metrics.reject_duplicate = result->rejected_duplicate;
    metrics.reject_rate_limited = result->rejected_rate_limited;
    metrics.intent_published = result->intents_published;
    metrics.bundle_rejected =
        result->rejected_invalid_settlement +
        result->rejected_bad_market_state +
        result->rejected_missing_snapshot +
        result->rejected_insufficient_depth +
        result->rejected_low_edge +
        result->rejected_duplicate +
        result->rejected_rate_limited;
    metrics.observe_scan_latency(scan_latency_ns);
}

}  // namespace

SignalEngine::SignalEngine(
    SignalConfig config,
    const IMarketSnapshotReader* snapshot_reader,
    const OracleArtifactReader* artifact_reader,
    const SettlementMaskChecker* settlement_checker,
    const VWAPPrecheck* vwap,
    const TheoreticalEdgeCalculator* edge_calculator,
    const OpportunityRanker* ranker,
    IIntentPublisher* publisher,
    IntentDeduper* deduper,
    IntentRateLimiter* rate_limiter
) : config_(config),
    snapshot_reader_(snapshot_reader),
    artifact_reader_(artifact_reader),
    settlement_checker_(settlement_checker),
    vwap_(vwap),
    edge_calculator_(edge_calculator),
    ranker_(ranker),
    publisher_(publisher),
    deduper_(deduper),
    default_rate_limiter_(config.max_intents_per_second),
    external_rate_limiter_(rate_limiter),
    scratch_(std::make_unique<SignalScratch>()) {
    snapshot_batch_reader_ =
        dynamic_cast<const ISnapshotBatchReader*>(snapshot_reader_);
    depth_batch_reader_ =
        dynamic_cast<const IDepthBatchReader*>(snapshot_reader_);
    const auto reserve_count = std::min<std::size_t>(
        config_.max_intents_per_scan,
        kSignalScratchMaxIntents
    );
    last_published_intents_.reserve(reserve_count);
    last_published_evidence_.reserve(reserve_count);
    last_published_evidence_indices_.reserve(reserve_count);
}

std::span<const OpportunityIntent> SignalEngine::last_published_intents()
    const noexcept {
    return last_published_intents_;
}

SignalEvidenceView SignalEngine::last_published_evidence_at(
    std::size_t index
) const noexcept {
    if (index >= last_published_evidence_indices_.size()) {
        return {};
    }
    const auto evidence_index = last_published_evidence_indices_[index];
    if (evidence_index >= last_published_evidence_.size()) {
        return {};
    }
    return last_published_evidence_[evidence_index].view();
}

SignalRunResult SignalEngine::scan_once(
    const SignalScanContext& context
) {
    using Clock = std::chrono::steady_clock;
    const auto scan_started = Clock::now();
    SignalRunResult result;
    scratch_->reset();
    last_published_intents_.clear();
    last_published_evidence_.clear();
    last_published_evidence_indices_.clear();
    auto store_evidence = [&](SignalPublishedEvidence evidence) {
        last_published_evidence_.push_back(std::move(evidence));
        return last_published_evidence_.size() - 1U;
    };
    auto finalize = [&]() {
        const auto elapsed = Clock::now() - scan_started;
        populate_metrics(
            &result,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    elapsed
                ).count()
            )
        );
        return result;
    };

    if (!snapshot_reader_ ||
        !artifact_reader_ ||
        !settlement_checker_ ||
        !vwap_ ||
        !edge_calculator_) {
        return finalize();
    }

    const auto publish_limit = std::min<std::uint32_t>(
        config_.max_intents_per_scan,
        kSignalScratchMaxIntents
    );
    auto try_publish = [&](OpportunityIntent intent, std::size_t evidence_index) {
        if (scratch_->publish_candidate_count >= publish_limit) {
            return;
        }
        (void)scratch_->push_publish_candidate(
            std::move(intent),
            evidence_index
        );
    };
    const auto bundle_fetch_start = Clock::now();
    const auto bundles = artifact_reader_->active_bundles();
    const auto runtime_plans = artifact_reader_->active_runtime_plans();
    result.stage_timings.bundle_scan_ns += elapsed_ns(bundle_fetch_start);
    std::uint64_t ordinal = 0;

    if ((depth_batch_reader_ || snapshot_batch_reader_) &&
        !runtime_plans.empty()) {
        for (const auto& plan : runtime_plans) {
            if (!plan.bundle) {
                continue;
            }
            const auto& bundle = *plan.bundle;
            const auto bundle_scan_start = Clock::now();
            ++ordinal;
            ++result.bundles_scanned;

            OpportunityIntent intent;
            intent.intent_id = make_intent_id(context, ordinal);
            intent.bundle_id = bundle.bundle_id;
            intent.guaranteed_payout_tick = bundle.guaranteed_payout_tick;
            intent.min_edge_tick = bundle.min_edge_tick;
            intent.oracle_artifact_version = artifact_reader_->artifact_version();
            intent.bundle_hash = plan.bundle_hash;
            intent.oracle_artifact_hash = plan.oracle_artifact_hash;
            intent.constraint_hash = plan.constraint_hash;
            copy_bundle_legs(bundle, &intent);
            copy_plan_asset_indices(plan, &intent);
            result.stage_timings.bundle_scan_ns += elapsed_ns(bundle_scan_start);

            const auto settlement_start = Clock::now();
            if (!settlement_checker_->is_valid(bundle, context)) {
                result.stage_timings.settlement_check_ns +=
                    elapsed_ns(settlement_start);
                intent.status = IntentStatus::RejectedInvalidSettlement;
                intent.reject_code = IntentRejectCode::InvalidSettlement;
                increment_counter_for_status(intent.status, &result);
                if (should_publish(config_, intent.status)) {
                    try_publish(
                        std::move(intent),
                        kSignalScratchNoEvidenceIndex
                    );
                }
                continue;
            }
            result.stage_timings.settlement_check_ns +=
                elapsed_ns(settlement_start);
            intent.valid_under_settlement = true;

            SignalPublishedEvidence evidence;
            SnapshotVersion snapshot_version;
            CostResult cost;
            if (depth_batch_reader_) {
                const auto snapshot_start = Clock::now();
                const auto depth = depth_batch_reader_->read_depth_for_plan(
                    plan,
                    config_,
                    context.now_monotonic_ns
                );
                const auto snapshot_total_ns = elapsed_ns(snapshot_start);
                result.stage_timings.snapshot_consistency_guard_ns +=
                    depth.snapshot_consistency_guard_ns;
                result.stage_timings.snapshot_reader_ns +=
                    snapshot_total_ns >= depth.snapshot_consistency_guard_ns
                        ? snapshot_total_ns -
                              depth.snapshot_consistency_guard_ns
                        : snapshot_total_ns;
                if (!depth.ok) {
                    intent.status = depth.rejection_status;
                    intent.reject_reason = depth.error;
                    intent.reject_code =
                        intent.status == IntentStatus::RejectedMissingSnapshot
                            ? IntentRejectCode::MissingSnapshot
                            : IntentRejectCode::BadMarketState;
                    increment_counter_for_status(intent.status, &result);
                    if (is_stale_snapshot_rejection(depth)) {
                        ++result.rejected_stale_snapshot;
                    }
                    if (is_snapshot_skew_rejection(depth)) {
                        ++result.rejected_snapshot_skew;
                    }
                    if (should_publish(config_, intent.status)) {
                        try_publish(
                            std::move(intent),
                            kSignalScratchNoEvidenceIndex
                        );
                    }
                    continue;
                }
                evidence = evidence_from_depth(
                    depth,
                    context.now_monotonic_ns
                );
                snapshot_version = depth.snapshot_version;
                ++result.vwap_checked;
                const auto vwap_start = Clock::now();
                cost = vwap_->price_runtime_plan(plan, depth);
                const auto vwap_total_ns = elapsed_ns(vwap_start);
                result.stage_timings.price_vector_builder_ns +=
                    cost.price_vector_builder_ns;
                result.stage_timings.vwap_precheck_ns +=
                    cost.vwap_precheck_ns != 0 ? cost.vwap_precheck_ns :
                                                 vwap_total_ns;
            } else {
                const auto snapshot_start = Clock::now();
                const auto snapshots = snapshot_batch_reader_->read_for_plan(
                    plan,
                    config_,
                    context.now_monotonic_ns
                );
                const auto snapshot_total_ns = elapsed_ns(snapshot_start);
                result.stage_timings.snapshot_consistency_guard_ns +=
                    snapshots.snapshot_consistency_guard_ns;
                result.stage_timings.snapshot_reader_ns +=
                    snapshot_total_ns >= snapshots.snapshot_consistency_guard_ns
                        ? snapshot_total_ns -
                              snapshots.snapshot_consistency_guard_ns
                        : snapshot_total_ns;
                if (!snapshots.ok) {
                    intent.status = snapshots.rejection_status;
                    intent.reject_reason = snapshots.error;
                    intent.reject_code =
                        intent.status == IntentStatus::RejectedMissingSnapshot
                            ? IntentRejectCode::MissingSnapshot
                            : IntentRejectCode::BadMarketState;
                    increment_counter_for_status(intent.status, &result);
                    if (is_stale_snapshot_rejection(snapshots)) {
                        ++result.rejected_stale_snapshot;
                    }
                    if (is_snapshot_skew_rejection(snapshots)) {
                        ++result.rejected_snapshot_skew;
                    }
                    if (should_publish(config_, intent.status)) {
                        try_publish(
                            std::move(intent),
                            kSignalScratchNoEvidenceIndex
                        );
                    }
                    continue;
                }
                evidence = evidence_from_batch(
                    snapshots,
                    context.now_monotonic_ns
                );
                snapshot_version = snapshots.snapshot_version;
                ++result.vwap_checked;
                const auto vwap_start = Clock::now();
                cost = vwap_->price_runtime_plan(plan, snapshots);
                const auto vwap_total_ns = elapsed_ns(vwap_start);
                result.stage_timings.price_vector_builder_ns +=
                    cost.price_vector_builder_ns;
                result.stage_timings.vwap_precheck_ns +=
                    cost.vwap_precheck_ns != 0 ? cost.vwap_precheck_ns :
                                                 vwap_total_ns;
            }
            intent.passed_quality_gate = true;
            intent.snapshot_version = snapshot_version.max_book_version;
            intent.snapshot_version_hash = snapshot_version.combined_hash;

            copy_cost_fields(cost, &intent);
            if (!cost.executable) {
                intent.status = status_from_cost_failure(cost.failure_reason);
                intent.reject_code =
                    intent.status == IntentStatus::RejectedMissingSnapshot
                        ? IntentRejectCode::MissingSnapshot
                        : IntentRejectCode::InsufficientDepth;
                increment_counter_for_status(intent.status, &result);
                if (should_publish(config_, intent.status)) {
                    try_publish(
                        std::move(intent),
                        kSignalScratchNoEvidenceIndex
                    );
                }
                continue;
            }

            ++result.edge_computed;
            const auto edge_start = Clock::now();
            const auto edge = edge_calculator_->calculate(
                bundle,
                cost,
                config_
            );
            result.stage_timings.edge_calculator_ns += elapsed_ns(edge_start);
            const auto intent_start = Clock::now();
            IntentBuilder intent_builder;
            intent = intent_builder.build(IntentBuildInput{
                .bundle = &bundle,
                .snapshot_version = snapshot_version,
                .cost = &cost,
                .edge = &edge,
                .now_ns = context.now_monotonic_ns,
                .ttl_ns = config_.intent_ttl_ns,
                .oracle_artifact_version = artifact_reader_->artifact_version(),
                .oracle_artifact_hash = plan.oracle_artifact_hash,
                .constraint_hash = plan.constraint_hash,
                .bundle_hash = plan.bundle_hash,
                .valid_under_settlement = true,
                .passed_quality_gate = true
            });
            copy_plan_asset_indices(plan, &intent);
            result.stage_timings.intent_builder_ns += elapsed_ns(intent_start);
            increment_counter_for_status(intent.status, &result);
            if (should_publish(config_, intent.status)) {
                try_publish(
                    std::move(intent),
                    store_evidence(std::move(evidence))
                );
            }
        }
    } else {
    for (const auto& bundle : bundles) {
        const auto bundle_scan_start = Clock::now();
        ++ordinal;
        ++result.bundles_scanned;

        OpportunityIntent intent;
        intent.intent_id = make_intent_id(context, ordinal);
        intent.bundle_id = bundle.bundle_id;
        intent.guaranteed_payout_tick = bundle.guaranteed_payout_tick;
        intent.min_edge_tick = bundle.min_edge_tick;
        intent.oracle_artifact_version = artifact_reader_->artifact_version();
        copy_bundle_legs(bundle, &intent);
        result.stage_timings.bundle_scan_ns += elapsed_ns(bundle_scan_start);

        const auto settlement_start = Clock::now();
        if (!settlement_checker_->is_valid(bundle, context)) {
            result.stage_timings.settlement_check_ns +=
                elapsed_ns(settlement_start);
            intent.status = IntentStatus::RejectedInvalidSettlement;
            increment_counter_for_status(intent.status, &result);
            if (should_publish(config_, intent.status)) {
                try_publish(
                    std::move(intent),
                    kSignalScratchNoEvidenceIndex
                );
            }
            continue;
        }
        result.stage_timings.settlement_check_ns +=
            elapsed_ns(settlement_start);
        intent.valid_under_settlement = true;

        const auto snapshot_start = Clock::now();
        const auto snapshots = snapshot_reader_->read_for_bundle(
            bundle,
            config_,
            context.now_monotonic_ns
        );
        const auto snapshot_total_ns = elapsed_ns(snapshot_start);
        result.stage_timings.snapshot_consistency_guard_ns +=
            snapshots.snapshot_consistency_guard_ns;
        result.stage_timings.snapshot_reader_ns +=
            snapshot_total_ns >= snapshots.snapshot_consistency_guard_ns
                ? snapshot_total_ns - snapshots.snapshot_consistency_guard_ns
                : snapshot_total_ns;
        if (!snapshots.ok) {
            intent.status = snapshots.rejection_status;
            intent.reject_reason = snapshots.error;
            increment_counter_for_status(intent.status, &result);
            if (is_stale_snapshot_rejection(snapshots)) {
                ++result.rejected_stale_snapshot;
            }
            if (is_snapshot_skew_rejection(snapshots)) {
                ++result.rejected_snapshot_skew;
            }
            if (should_publish(config_, intent.status)) {
                try_publish(
                    std::move(intent),
                    kSignalScratchNoEvidenceIndex
                );
            }
            continue;
        }
        const auto evidence = evidence_from_snapshots(
            snapshots,
            context.now_monotonic_ns
        );
        intent.passed_quality_gate = true;
        intent.snapshot_version =
            snapshots.snapshot_version.max_book_version;
        intent.snapshot_version_hash =
            snapshots.snapshot_version.combined_hash;

        ++result.vwap_checked;
        const auto vwap_start = Clock::now();
        const auto cost = vwap_->price_bundle(bundle, snapshots.snapshots);
        const auto vwap_total_ns = elapsed_ns(vwap_start);
        result.stage_timings.price_vector_builder_ns +=
            cost.price_vector_builder_ns;
        result.stage_timings.vwap_precheck_ns +=
            cost.vwap_precheck_ns != 0 ? cost.vwap_precheck_ns : vwap_total_ns;
        copy_cost_fields(cost, &intent);
        if (!cost.executable) {
            intent.status = status_from_cost_failure(cost.failure_reason);
            increment_counter_for_status(intent.status, &result);
            if (should_publish(config_, intent.status)) {
                try_publish(
                    std::move(intent),
                    kSignalScratchNoEvidenceIndex
                );
            }
            continue;
        }

        ++result.edge_computed;
        const auto edge_start = Clock::now();
        const auto edge = edge_calculator_->calculate(bundle, cost, config_);
        result.stage_timings.edge_calculator_ns += elapsed_ns(edge_start);
        const auto intent_start = Clock::now();
        IntentBuilder intent_builder;
        intent = intent_builder.build(IntentBuildInput{
            .bundle = &bundle,
            .snapshot = &snapshots,
            .cost = &cost,
            .edge = &edge,
            .now_ns = context.now_monotonic_ns,
            .ttl_ns = config_.intent_ttl_ns,
            .oracle_artifact_version = artifact_reader_->artifact_version(),
            .oracle_artifact_hash = artifact_reader_->artifact_hash(),
            .constraint_hash = artifact_reader_->constraint_hash(),
            .bundle_hash = 0,
            .valid_under_settlement = true,
            .passed_quality_gate = true
        });
        result.stage_timings.intent_builder_ns += elapsed_ns(intent_start);
        increment_counter_for_status(intent.status, &result);
        if (should_publish(config_, intent.status)) {
            try_publish(
                std::move(intent),
                store_evidence(std::move(evidence))
            );
        }
    }
    }

    for (std::uint16_t i = 0; i < scratch_->publish_candidate_count; ++i) {
        auto& candidate = scratch_->publish_candidates[i];
        complete_intent_lifecycle(
            &candidate.intent,
            config_,
            *artifact_reader_,
            context.now_monotonic_ns
        );
    }

    const auto rank_start = Clock::now();
    if (ranker_) {
        std::sort(
            scratch_->publish_candidates.begin(),
            scratch_->publish_candidates.begin() +
                scratch_->publish_candidate_count,
            candidate_less
        );
    }
    result.stage_timings.bundle_scan_ns += elapsed_ns(rank_start);

    const auto dedupe_start = Clock::now();
    std::uint16_t deduped_count = 0;
    for (std::uint16_t i = 0; i < scratch_->publish_candidate_count; ++i) {
        auto& candidate = scratch_->publish_candidates[i];
        const auto& intent = candidate.intent;
        if (intent.status == IntentStatus::PaperOpportunity &&
            deduper_ &&
            (intent.idempotency_hash != 0
                 ? deduper_->seen_recently(
                       intent.idempotency_hash,
                       context.now_monotonic_ns
                   )
                 : deduper_->seen_recently(
                       intent.idempotency_key,
                       context.now_monotonic_ns
                   ))) {
            ++result.duplicate_intents;
            ++result.rejected_duplicate;
            continue;
        }
        if (intent.status == IntentStatus::PaperOpportunity && deduper_) {
            if (intent.idempotency_hash != 0) {
                deduper_->mark_seen(
                    intent.idempotency_hash,
                    context.now_monotonic_ns
                );
            } else {
                deduper_->mark_seen(
                    intent.idempotency_key,
                    context.now_monotonic_ns
                );
            }
        }
        if (deduped_count != i) {
            scratch_->publish_candidates[deduped_count] =
                std::move(candidate);
        }
        ++deduped_count;
    }
    scratch_->publish_candidate_count = deduped_count;
    result.stage_timings.dedupe_ns += elapsed_ns(dedupe_start);

    const auto rate_start = Clock::now();
    std::uint16_t allowed_count = 0;
    auto* rate_limiter = external_rate_limiter_ == nullptr
        ? &default_rate_limiter_
        : external_rate_limiter_;
    for (std::uint16_t i = 0; i < scratch_->publish_candidate_count; ++i) {
        auto& candidate = scratch_->publish_candidates[i];
        const auto& intent = candidate.intent;
        if (rate_limiter &&
            !rate_limiter->allow(context.now_monotonic_ns)) {
            ++result.rate_limited;
            ++result.rejected_rate_limited;
            continue;
        }
        if (allowed_count != i) {
            scratch_->publish_candidates[allowed_count] =
                std::move(candidate);
        }
        ++allowed_count;
    }
    scratch_->publish_candidate_count = allowed_count;
    result.stage_timings.rate_limiter_ns += elapsed_ns(rate_start);

    for (std::uint16_t i = 0; i < scratch_->publish_candidate_count; ++i) {
        const auto& candidate = scratch_->publish_candidates[i];
        last_published_intents_.push_back(candidate.intent);
        last_published_evidence_indices_.push_back(candidate.evidence_index);
    }
    result.output_hash = hash_published_intents(last_published_intents_);

    const auto publish_start = Clock::now();
    if (publisher_) {
        const bool materialize_strings =
            publisher_->requires_materialized_strings();
        for (std::size_t i = 0; i < last_published_intents_.size(); ++i) {
            const auto evidence_view = last_published_evidence_at(i);
            if (materialize_strings) {
                auto intent = last_published_intents_[i];
                materialize_intent_strings(&intent);
                publisher_->publish(intent, evidence_view);
            } else {
                publisher_->publish(
                    last_published_intents_[i],
                    evidence_view
                );
            }
            ++result.intents_published;
        }
    }
    result.stage_timings.publisher_ns += elapsed_ns(publish_start);

    return finalize();
}

}  // namespace trading_engine::signal
