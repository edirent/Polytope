#include "engine/signal/core/SignalEngine.h"

#include "engine/signal/publish/IntentBuilder.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace trading_engine::signal {

namespace {

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

void copy_cost_fields(
    const CostResult& cost,
    OpportunityIntent* intent
) {
    intent->enough_depth = cost.executable;
    intent->bundle_qty = cost.bundle_qty;
    intent->estimated_cost_tick = cost.total_cost_tick;
    intent->slippage_buffer_tick = cost.max_leg_slippage_tick;
    intent->leg_count = std::max<std::uint16_t>(
        intent->leg_count,
        static_cast<std::uint16_t>(
            std::min<std::size_t>(cost.legs.size(), kMaxIntentLegs)
        )
    );
    for (std::uint16_t i = 0; i < intent->leg_count && i < cost.legs.size(); ++i) {
        const auto& source = cost.legs[i];
        if (source.asset_id.empty()) {
            continue;
        }
        auto& target = intent->legs[i];
        target.asset_id = source.asset_id;
        target.quantity_lots = source.requested_qty_lots;
        target.estimated_vwap_tick = source.vwap_price_tick;
        target.worst_price_tick = source.worst_price_tick;
        target.estimated_cost_tick = source.total_cost_tick;
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
    external_rate_limiter_(rate_limiter) {}

SignalRunResult SignalEngine::scan_once(
    const SignalScanContext& context
) {
    using Clock = std::chrono::steady_clock;
    const auto scan_started = Clock::now();
    SignalRunResult result;
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

    std::vector<OpportunityIntent> publishable;
    const auto bundles = artifact_reader_->active_bundles();
    std::uint64_t ordinal = 0;

    for (const auto& bundle : bundles) {
        ++ordinal;
        ++result.bundles_scanned;

        OpportunityIntent intent;
        intent.intent_id = make_intent_id(context, ordinal);
        intent.bundle_id = bundle.bundle_id;
        intent.guaranteed_payout_tick = bundle.guaranteed_payout_tick;
        intent.min_edge_tick = bundle.min_edge_tick;
        intent.oracle_artifact_version = artifact_reader_->artifact_version();
        copy_bundle_legs(bundle, &intent);

        if (!settlement_checker_->is_valid(bundle, context)) {
            intent.status = IntentStatus::RejectedInvalidSettlement;
            increment_counter_for_status(intent.status, &result);
            if (should_publish(config_, intent.status) &&
                publishable.size() < config_.max_intents_per_scan) {
                publishable.push_back(std::move(intent));
            }
            continue;
        }
        intent.valid_under_settlement = true;

        const auto snapshots = snapshot_reader_->read_for_bundle(
            bundle,
            config_,
            context.now_monotonic_ns
        );
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
            if (should_publish(config_, intent.status) &&
                publishable.size() < config_.max_intents_per_scan) {
                publishable.push_back(std::move(intent));
            }
            continue;
        }
        intent.passed_quality_gate = true;
        intent.snapshot_version =
            snapshots.snapshot_version.max_book_version;
        intent.snapshot_version_hash =
            snapshots.snapshot_version.combined_hash;

        ++result.vwap_checked;
        const auto cost = vwap_->price_bundle(bundle, snapshots.snapshots);
        copy_cost_fields(cost, &intent);
        if (!cost.executable) {
            intent.status = status_from_cost_failure(cost.failure_reason);
            increment_counter_for_status(intent.status, &result);
            if (should_publish(config_, intent.status) &&
                publishable.size() < config_.max_intents_per_scan) {
                publishable.push_back(std::move(intent));
            }
            continue;
        }

        ++result.edge_computed;
        const auto edge = edge_calculator_->calculate(bundle, cost, config_);
        IntentBuilder intent_builder;
        intent = intent_builder.build(IntentBuildInput{
            .bundle = &bundle,
            .snapshot = &snapshots,
            .cost = &cost,
            .edge = &edge,
            .now_ns = context.now_monotonic_ns,
            .ttl_ns = 0,
            .oracle_artifact_version = artifact_reader_->artifact_version(),
            .oracle_artifact_hash = artifact_reader_->artifact_hash(),
            .constraint_hash = artifact_reader_->constraint_hash(),
            .bundle_hash = 0,
            .valid_under_settlement = true,
            .passed_quality_gate = true
        });
        increment_counter_for_status(intent.status, &result);
        if (should_publish(config_, intent.status) &&
            publishable.size() < config_.max_intents_per_scan) {
            publishable.push_back(std::move(intent));
        }
    }

    if (ranker_) {
        ranker_->rank(&publishable);
    }

    std::vector<OpportunityIntent> deduped;
    deduped.reserve(publishable.size());
    for (const auto& intent : publishable) {
        if (intent.status == IntentStatus::PaperOpportunity &&
            deduper_ &&
            deduper_->seen_recently(
                intent.idempotency_key,
                context.now_monotonic_ns
            )) {
            ++result.duplicate_intents;
            ++result.rejected_duplicate;
            continue;
        }
        if (intent.status == IntentStatus::PaperOpportunity && deduper_) {
            deduper_->mark_seen(
                intent.idempotency_key,
                context.now_monotonic_ns
            );
        }
        deduped.push_back(intent);
    }

    std::vector<OpportunityIntent> allowed;
    allowed.reserve(deduped.size());
    auto* rate_limiter = external_rate_limiter_ == nullptr
        ? &default_rate_limiter_
        : external_rate_limiter_;
    for (const auto& intent : deduped) {
        if (rate_limiter &&
            !rate_limiter->allow(context.now_monotonic_ns)) {
            ++result.rate_limited;
            ++result.rejected_rate_limited;
            continue;
        }
        allowed.push_back(intent);
    }

    result.output_hash = hash_published_intents(allowed);

    if (publisher_) {
        for (const auto& intent : allowed) {
            publisher_->publish(intent);
            ++result.intents_published;
        }
    }

    return finalize();
}

}  // namespace trading_engine::signal
