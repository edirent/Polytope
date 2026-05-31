#include "engine/signal/core/SignalWorkflow.h"

namespace trading_engine::signal {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hash_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= (value >> shift) & 0xffU;
        *hash *= kFnvPrime;
    }
}

void hash_i64(std::uint64_t* hash, std::int64_t value) noexcept {
    hash_u64(hash, static_cast<std::uint64_t>(value));
}

void hash_bool(std::uint64_t* hash, bool value) noexcept {
    *hash ^= value ? 1U : 0U;
    *hash *= kFnvPrime;
}

}  // namespace

bool SettlementMaskChecker::is_valid(
    const CandidateBundle& bundle,
    const SignalScanContext& context
) const noexcept {
    if (!context.settlement_masks_available) {
        return true;
    }

    if ((bundle.required_true_mask & ~context.current_true_mask) != 0) {
        return false;
    }
    if ((bundle.required_false_mask & ~context.current_false_mask) != 0) {
        return false;
    }

    const auto resolved_mask =
        context.current_true_mask | context.current_false_mask;
    return (bundle.invalid_mask & resolved_mask) == 0;
}

void increment_counter_for_status(
    IntentStatus status,
    SignalRunResult* result
) noexcept {
    if (!result) {
        return;
    }

    switch (status) {
        case IntentStatus::RejectedInvalidSettlement:
            ++result->rejected_invalid_settlement;
            break;
        case IntentStatus::RejectedBadMarketState:
            ++result->rejected_bad_market_state;
            break;
        case IntentStatus::RejectedMissingSnapshot:
            ++result->rejected_missing_snapshot;
            break;
        case IntentStatus::RejectedInsufficientDepth:
            ++result->rejected_insufficient_depth;
            break;
        case IntentStatus::RejectedLowEdge:
            ++result->rejected_low_edge;
            break;
        case IntentStatus::DuplicateIntent:
            ++result->duplicate_intents;
            ++result->rejected_duplicate;
            break;
        case IntentStatus::PaperOpportunity:
            ++result->paper_opportunities;
            break;
        case IntentStatus::CandidateOnly:
            break;
    }
}

std::uint64_t hash_published_intents(
    std::span<const OpportunityIntent> intents
) noexcept {
    if (intents.empty()) {
        return 0;
    }

    auto hash = kFnvOffset;
    for (const auto& intent : intents) {
        hash_u64(&hash, intent.intent_id);
        hash_u64(&hash, intent.bundle_id);
        hash_u64(&hash, static_cast<std::uint64_t>(intent.status));
        hash_bool(&hash, intent.valid_under_settlement);
        hash_bool(&hash, intent.passed_quality_gate);
        hash_bool(&hash, intent.enough_depth);
        hash_i64(&hash, intent.guaranteed_payout_tick);
        hash_i64(&hash, intent.estimated_cost_tick);
        hash_i64(&hash, intent.estimated_fee_tick);
        hash_i64(&hash, intent.latency_buffer_tick);
        hash_i64(&hash, intent.estimated_edge_tick);
        hash_i64(&hash, intent.min_edge_tick);
        hash_u64(&hash, intent.oracle_artifact_hash);
        hash_u64(&hash, intent.constraint_hash);
        hash_u64(&hash, intent.bundle_hash);
        hash_u64(&hash, intent.snapshot_version);
        hash_i64(&hash, intent.bundle_qty);
        hash_i64(&hash, intent.original_bundle_qty);
        hash_i64(&hash, intent.unit_edge_tick);
        hash_i64(&hash, intent.total_edge_tick);
        hash_i64(&hash, intent.edge_bps);
        hash_i64(&hash, intent.slippage_buffer_tick);
        hash_i64(&hash, intent.max_leg_slippage_tick);
        hash_u64(&hash, intent.created_ts_ns);
        hash_u64(&hash, intent.expires_at_ns);
        hash_u64(&hash, intent.leg_count);
        for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
            const auto& leg = intent.legs[i];
            hash_u64(&hash, static_cast<std::uint64_t>(leg.side));
            hash_i64(&hash, leg.quantity_lots);
            hash_i64(&hash, leg.estimated_vwap_tick);
            hash_i64(&hash, leg.worst_price_tick);
            hash_i64(&hash, leg.estimated_cost_tick);
            hash_bool(&hash, leg.enough_depth);
        }
        hash_u64(&hash, intent.snapshot_version_hash);
        hash_u64(&hash, intent.oracle_artifact_version);
    }
    return hash;
}

}  // namespace trading_engine::signal
