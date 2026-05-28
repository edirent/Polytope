#include "engine/signal/core/SignalEngine.h"

#include <algorithm>
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
    intent->enough_depth = cost.enough_depth;
    intent->estimated_cost_tick = cost.total_cost_tick;
    intent->leg_count = std::max(intent->leg_count, cost.filled_leg_count);
    for (std::uint16_t i = 0; i < intent->leg_count; ++i) {
        if (cost.priced_legs[i].asset_id.empty()) {
            continue;
        }
        intent->legs[i] = cost.priced_legs[i];
    }
}

void copy_edge_fields(
    const EdgeBreakdown& edge,
    OpportunityIntent* intent
) {
    intent->guaranteed_payout_tick = edge.guaranteed_payout_tick;
    intent->estimated_cost_tick = edge.total_cost_tick;
    intent->estimated_fee_tick = edge.fee_tick;
    intent->latency_buffer_tick = edge.latency_buffer_tick;
    intent->estimated_edge_tick = edge.estimated_edge_tick;
    intent->min_edge_tick = edge.min_edge_tick;
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

[[nodiscard]] std::uint64_t snapshot_hash(
    const std::vector<MarketStateSnapshot>& snapshots
) noexcept {
    std::uint64_t out = 0;
    for (const auto& snapshot : snapshots) {
        out ^= snapshot.state_hash + 0x9e3779b97f4a7c15ULL + (out << 6U) +
               (out >> 2U);
    }
    return out;
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
    IIntentPublisher* publisher
) : config_(config),
    snapshot_reader_(snapshot_reader),
    artifact_reader_(artifact_reader),
    settlement_checker_(settlement_checker),
    vwap_(vwap),
    edge_calculator_(edge_calculator),
    ranker_(ranker),
    publisher_(publisher) {}

SignalRunResult SignalEngine::scan_once(
    const SignalScanContext& context
) {
    SignalRunResult result;
    if (!snapshot_reader_ ||
        !artifact_reader_ ||
        !settlement_checker_ ||
        !vwap_ ||
        !edge_calculator_) {
        return result;
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

        const auto snapshots =
            snapshot_reader_->read_for_bundle(bundle, config_);
        if (!snapshots.ok) {
            intent.status = snapshots.rejection_status;
            intent.reject_reason = snapshots.error;
            increment_counter_for_status(intent.status, &result);
            if (should_publish(config_, intent.status) &&
                publishable.size() < config_.max_intents_per_scan) {
                publishable.push_back(std::move(intent));
            }
            continue;
        }
        intent.passed_quality_gate = true;
        intent.snapshot_version_hash = snapshot_hash(snapshots.snapshots);

        const auto cost = vwap_->price_bundle(bundle, snapshots.snapshots);
        copy_cost_fields(cost, &intent);
        if (!cost.enough_depth) {
            intent.status = status_from_cost_failure(cost.failure_reason);
            increment_counter_for_status(intent.status, &result);
            if (should_publish(config_, intent.status) &&
                publishable.size() < config_.max_intents_per_scan) {
                publishable.push_back(std::move(intent));
            }
            continue;
        }

        const auto edge = edge_calculator_->calculate(bundle, cost);
        copy_edge_fields(edge, &intent);
        intent.status = edge.above_threshold
            ? IntentStatus::PaperOpportunity
            : IntentStatus::RejectedLowEdge;
        increment_counter_for_status(intent.status, &result);
        if (should_publish(config_, intent.status) &&
            publishable.size() < config_.max_intents_per_scan) {
            publishable.push_back(std::move(intent));
        }
    }

    if (ranker_) {
        ranker_->rank(&publishable);
    }
    result.output_hash = hash_published_intents(publishable);

    if (publisher_) {
        for (const auto& intent : publishable) {
            publisher_->publish(intent);
            ++result.intents_published;
        }
    }

    return result;
}

}  // namespace trading_engine::signal
