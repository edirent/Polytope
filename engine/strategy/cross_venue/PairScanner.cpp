#include "engine/strategy/cross_venue/PairScanner.h"
#include "engine/common/math/VwapMath.h"
#include "engine/common/math/FractionalVwap.h"
#include <algorithm>
#include <limits>

namespace trading_engine::cross_venue {
CrossVenueOpportunity PairScanner::quote(const ApprovedPair& p, const std::array<VersionedBook, 2>& b,
                                         const FeeQuote& f, const PairPolicy& policy, std::uint64_t now) {
    validate_books(p, b, policy, now);
    require(policy.min_quantity > 0 && policy.quantity_step > 0 && policy.min_profit >= 0 &&
            policy.min_profit_bps >= 0 && policy.min_profit_bps <= 10'000 && policy.max_single_leg_loss > 0, "invalid policy");
    require(f.qty > 0 && f.qty <= max_quantity && f.qty >= policy.min_quantity && f.qty % policy.quantity_step == 0 &&
            std::find(policy.quantities.begin(), policy.quantities.end(), f.qty) != policy.quantities.end(), "unsupported quantity");
    require(!f.version.empty() && f.pair_artifact_hash == p.artifact_hash && now >= f.valid_from_ns && now < f.valid_until_ns, "missing/expired fee quote");
    CrossVenueOpportunity o;
    o.pair_id = p.pair_id; o.artifact_hash = p.artifact_hash; o.approval_revision = p.approval_revision;
    o.fee_version = f.version; o.fees = f.fees; o.slippage = f.slippage;
    auto& intent = o.intent;
    intent.bundle_qty = intent.original_bundle_qty = f.qty;
    intent.guaranteed_payout_tick = multiply(unit, f.qty);
    intent.leg_count = 2;
    Money budget = 0;
    std::string proof = field(p.artifact_hash) + field(std::to_string(f.qty)) + field(f.version);
    for (std::size_t i = 0; i < 2; ++i) {
        require(f.fees[i] >= 0 && f.slippage[i] >= 0, "negative fee/reserve");
        const auto cost = common::math::buy_vwap_exact(b[i], f.qty);
        require(cost.ok, "insufficient depth");
        o.costs[i] = cost.total_cost_tick; o.evidence[i] = b[i].evidence;
        budget = add(budget, add(o.costs[i], add(f.fees[i], f.slippage[i])));
        auto& leg = intent.legs[i];
        leg.asset_id = b[i].key.identity(); leg.market_id = field(venue_name(b[i].key.venue)) + field(b[i].key.market_id);
        leg.asset_index = static_cast<std::uint32_t>(i);
        leg.quantity_lots = leg.requested_qty_lots = f.qty;
        leg.executable_qty_lots = cost.executable_qty_lots;
        leg.estimated_cost_tick = cost.total_cost_tick; leg.estimated_vwap_tick = cost.vwap_tick;
        leg.worst_price_tick = cost.worst_price_tick; leg.enough_depth = true;
        intent.estimated_cost_tick = add(intent.estimated_cost_tick, o.costs[i]);
        intent.estimated_fee_tick = add(intent.estimated_fee_tick, f.fees[i]);
        intent.slippage_buffer_tick = add(intent.slippage_buffer_tick, f.slippage[i]);
        const auto& e = b[i].evidence;
        for (const auto& v : {std::to_string(e.snapshot_version), std::to_string(e.sequence), std::to_string(e.recv_monotonic_ns),
             std::to_string(e.source_ts_ns), std::to_string(e.raw_log_offset), e.raw_hash, std::to_string(b[i].depth.snapshot_version_hash), std::to_string(o.costs[i]),
             std::to_string(f.fees[i]), std::to_string(f.slippage[i])}) proof += field(v);
    }
    intent.estimated_edge_tick = intent.total_edge_tick = add(intent.guaranteed_payout_tick, -budget);
    require(intent.total_edge_tick > 0, "nonpositive indicative edge");
    intent.min_edge_tick = std::max(policy.min_profit, static_cast<Money>((static_cast<__int128>(budget) * policy.min_profit_bps + 9999) / 10000));
    intent.unit_edge_tick = intent.total_edge_tick / f.qty;
    intent.edge_bps = static_cast<std::int64_t>(static_cast<__int128>(intent.total_edge_tick) * 10000 / std::max<Money>(budget, 1));
    // Include policy and fee validity so changing either cannot reuse an ID.
    for (const auto& v : {std::to_string(policy.max_age_ns), std::to_string(policy.max_skew_ns), std::to_string(policy.max_total_payout),
         std::to_string(policy.max_single_leg_loss), std::to_string(policy.min_profit), std::to_string(policy.min_profit_bps),
         std::to_string(policy.min_quantity), std::to_string(policy.quantity_step), std::to_string(f.valid_from_ns), std::to_string(f.valid_until_ns)}) proof += field(v);
    if(policy.asof_enabled)proof+=field("ASOF_V012")+field(std::to_string(policy.asof.liveness_ns))+field(std::to_string(policy.asof.authority_ns))+field(std::to_string(policy.asof.change_ns))+field(std::to_string(policy.asof.research_only));
    if(policy.max_authoritative_age_ns || policy.max_liveness_age_ns) {
        proof+=field(std::to_string(policy.max_authoritative_age_ns))+field(std::to_string(policy.max_liveness_age_ns));
        for(const auto& v:b) proof+=field(std::to_string(v.evidence.last_authoritative_snapshot_ns))+field(std::to_string(v.evidence.last_liveness_ns));
    }
    o.candidate_id = sha256(proof);
    intent.intent_id = intent.idempotency_hash = compact_hash(o.candidate_id);
    intent.bundle_id = compact_hash(p.pair_id); intent.idempotency_key = o.candidate_id;
    intent.oracle_artifact_hash = compact_hash(p.artifact_hash); intent.oracle_artifact_version = p.approval_revision;
    intent.snapshot_version_hash = compact_hash(proof); intent.proof_ref = p.artifact_hash;
    intent.valid_under_settlement = intent.passed_quality_gate = intent.enough_depth = true;
    intent.created_ts_ns = now;
    const auto received = policy.max_authoritative_age_ns ? now : std::min(b[0].evidence.recv_monotonic_ns, b[1].evidence.recv_monotonic_ns);
    require(policy.max_age_ns < std::numeric_limits<std::uint64_t>::max() - received, "expiry overflow");
    intent.expires_at_ns = std::min(f.valid_until_ns, received + policy.max_age_ns + 1);
    return o;
}
}  // namespace trading_engine::cross_venue
