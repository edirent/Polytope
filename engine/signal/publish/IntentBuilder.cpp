#include "engine/signal/publish/IntentBuilder.h"

#include "engine/signal/publish/IntentId.h"
#include "oracle/bundles/BundleHash.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace trading_engine::signal {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hash_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
        *hash *= kFnvPrime;
    }
}

void hash_i64(std::uint64_t* hash, std::int64_t value) noexcept {
    hash_u64(hash, static_cast<std::uint64_t>(value));
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

[[nodiscard]] std::uint64_t effective_bundle_hash(
    const IntentBuildInput& input
) noexcept {
    if (input.bundle_hash != 0 || !input.bundle) {
        return input.bundle_hash;
    }
    return trading_engine::oracle::hash_candidate_bundle(*input.bundle);
}

[[nodiscard]] std::uint64_t effective_artifact_hash(
    const IntentBuildInput& input,
    std::uint64_t bundle_hash
) noexcept {
    if (input.oracle_artifact_hash != 0) {
        return input.oracle_artifact_hash;
    }
    std::uint64_t hash = kFnvOffset;
    hash_u64(&hash, input.oracle_artifact_version);
    hash_u64(&hash, input.constraint_hash);
    hash_u64(&hash, bundle_hash);
    return hash;
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

void copy_priced_legs(
    const CostResult& cost,
    OpportunityIntent* intent
) {
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

[[nodiscard]] std::string proof_ref(
    const OpportunityIntent& intent
) {
    return "oracle:" + hex_u64(intent.oracle_artifact_hash) +
           ":constraint:" + hex_u64(intent.constraint_hash) +
           ":bundle:" + hex_u64(intent.bundle_hash) +
           ":snapshot:" + hex_u64(intent.snapshot_version_hash);
}

}  // namespace

std::string hex_u64(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

std::uint64_t make_intent_id(
    const IntentIdentityInput& input
) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_u64(&hash, input.bundle_id);
    hash_u64(&hash, input.bundle_hash);
    hash_u64(&hash, input.snapshot_version_hash);
    hash_i64(&hash, input.bundle_qty);
    hash_i64(&hash, input.unit_edge_tick);
    return hash;
}

std::string make_idempotency_key(
    const IntentIdentityInput& input
) {
    return hex_u64(make_intent_id(input));
}

OpportunityIntent IntentBuilder::build(
    const IntentBuildInput& input
) const {
    OpportunityIntent intent;
    if (!input.bundle || !input.snapshot || !input.cost || !input.edge) {
        intent.status = IntentStatus::CandidateOnly;
        return intent;
    }

    const auto& bundle = *input.bundle;
    const auto& snapshot = *input.snapshot;
    const auto& cost = *input.cost;
    const auto& edge = *input.edge;

    intent.bundle_id = bundle.bundle_id;
    intent.status = edge.passed
        ? IntentStatus::PaperOpportunity
        : IntentStatus::RejectedLowEdge;
    intent.valid_under_settlement = input.valid_under_settlement;
    intent.passed_quality_gate = input.passed_quality_gate;
    intent.enough_depth = cost.executable;

    intent.oracle_artifact_version = input.oracle_artifact_version;
    intent.bundle_hash = effective_bundle_hash(input);
    intent.constraint_hash = input.constraint_hash;
    intent.oracle_artifact_hash =
        effective_artifact_hash(input, intent.bundle_hash);

    intent.snapshot_version = snapshot.snapshot_version.max_book_version;
    intent.snapshot_version_hash = snapshot.snapshot_version.combined_hash;

    intent.bundle_qty = edge.bundle_qty;
    intent.guaranteed_payout_tick = saturating_mul_i64(
        edge.guaranteed_payout_per_bundle_tick,
        edge.bundle_qty
    );
    intent.estimated_cost_tick = cost.total_cost_tick;
    intent.estimated_fee_tick = saturating_mul_i64(
        edge.fee_per_bundle_tick,
        edge.bundle_qty
    );
    intent.latency_buffer_tick = saturating_mul_i64(
        edge.latency_buffer_per_bundle_tick,
        edge.bundle_qty
    );
    intent.slippage_buffer_tick = saturating_mul_i64(
        edge.slippage_buffer_per_bundle_tick,
        edge.bundle_qty
    );
    intent.unit_edge_tick = edge.unit_edge_tick;
    intent.total_edge_tick = edge.total_edge_tick;
    intent.estimated_edge_tick = edge.total_edge_tick;
    intent.edge_bps = edge.edge_bps;
    intent.min_edge_tick = bundle.min_edge_tick;

    intent.created_ts_ns = input.now_ns;
    intent.expires_at_ns = saturating_add_u64(input.now_ns, input.ttl_ns);

    copy_bundle_legs(bundle, &intent);
    copy_priced_legs(cost, &intent);

    const IntentIdentityInput identity{
        .bundle_id = intent.bundle_id,
        .bundle_hash = intent.bundle_hash,
        .snapshot_version_hash = intent.snapshot_version_hash,
        .bundle_qty = intent.bundle_qty,
        .unit_edge_tick = intent.unit_edge_tick
    };
    intent.intent_id = make_intent_id(identity);
    intent.idempotency_key = make_idempotency_key(identity);
    intent.proof_ref = proof_ref(intent);
    return intent;
}

}  // namespace trading_engine::signal
