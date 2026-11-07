#include "engine/state/cross_venue/SynchronizedBookStore.h"
#include <algorithm>
#include <limits>
#include "engine/state/cross_venue/JointBookAsOf.h"

namespace trading_engine::cross_venue {
bool SynchronizedBookStore::apply(const BookUpdate& u) {
    auto& b = books_[u.key.identity()];
    auto invalidate = [&](const char* reason) { b.view.depth.usable_for_depth = false; b.view.depth.recovering = true; b.view.evidence.seq_gap = true; b.view.evidence.health_reason=reason;return false; };
    if (u.evidence.recv_monotonic_ns == 0 || u.evidence.recv_monotonic_ns < b.last_packet_recv ||
        !valid_sha256(u.evidence.raw_hash)) return invalidate("INVALID_TIME_OR_RAW_HASH");
    b.last_packet_recv = u.evidence.recv_monotonic_ns;
    if (u.kind == BookUpdate::Kind::Disconnect || u.kind == BookUpdate::Kind::Halt) {
        b.view.evidence = u.evidence; b.view.evidence.stale = true;
        b.view.evidence.halted = u.kind == BookUpdate::Kind::Halt;
        return invalidate("DISCONNECTED_OR_HALTED");
    }
    if (u.kind == BookUpdate::Kind::Delta && (!b.initialized || b.view.depth.recovering || b.view.fractional_quantities != u.fractional_quantities ||
        u.evidence.connection_epoch != b.view.evidence.connection_epoch ||
        b.view.evidence.sequence == std::numeric_limits<std::uint64_t>::max() ||
        (!u.subscription_sequence_validated && u.evidence.sequence != b.view.evidence.sequence + 1))) return invalidate("DELTA_BEFORE_SNAPSHOT_OR_SEQUENCE_GAP");
    if (u.evidence.sequence == 0 || !u.complete) return invalidate("INCOMPLETE_OR_UNSEQUENCED_UPDATE");
    auto bids = u.kind == BookUpdate::Kind::Snapshot ? std::map<Money, std::int64_t>{} : b.bids;
    auto asks = u.kind == BookUpdate::Kind::Snapshot ? std::map<Money, std::int64_t>{} : b.asks;
    auto update = [&](auto& side, const auto& levels) {
        std::map<Money, bool> seen;
        for (auto [price, qty] : levels) {
            if ((u.fractional_quantities ? (price < 0 || price > unit) : (price <= 0 || price >= unit)) || qty < 0 || qty > (u.fractional_quantities ? multiply(max_quantity,unit) : max_quantity) || !seen.emplace(price, true).second) return false;
            if(!retain_observed_depth_ && u.kind==BookUpdate::Kind::Delta && b.view.bounded_prefix && !side.empty()) {
                const bool bid=&side==&bids;
                if((bid && price<side.begin()->first) || (!bid && price>side.rbegin()->first))continue;
            }
            if (qty == 0) side.erase(price); else side[price] = qty;
        }
        if(u.bounded_prefix && !retain_observed_depth_){const bool bid=&side==&bids;while(side.size()>state::kMaxSnapshotDepth)side.erase(bid?side.begin():std::prev(side.end()));}
        return side.size() <= (u.bounded_prefix && retain_observed_depth_ ? 10000 : state::kMaxSnapshotDepth);
    };
    if (!update(bids, u.bids) || !update(asks, u.asks))return invalidate("INVALID_LEVELS_OR_DEPTH_BOUND");
    if(!allow_empty_sides_ && (bids.empty() || asks.empty()))return invalidate("EMPTY_SIDE");
    if(!bids.empty() && !asks.empty() && bids.rbegin()->first >= asks.begin()->first)return invalidate("CROSSED_OR_LOCKED_BOOK");
    auto version = b.view.evidence.snapshot_version + 1;
    require(version != 0, "book version overflow");
    std::string state_bytes = field(u.key.identity()) + field(std::to_string(b.view.depth.snapshot_version_hash)) + field(u.evidence.raw_hash);
    const bool changed = !b.initialized || bids != b.bids || asks != b.asks;
    const auto previous_change = b.view.evidence.last_book_change_ns;
    const auto previous_authoritative = b.view.evidence.last_authoritative_snapshot_ns;
    b.bids = std::move(bids); b.asks = std::move(asks); b.initialized = true;
    b.view = {}; b.view.key = u.key; b.view.evidence = u.evidence;
    b.view.evidence.snapshot_version = version; b.view.evidence.stale = false;
    b.view.fractional_quantities = u.fractional_quantities;
    b.view.bounded_prefix = u.bounded_prefix;
    if (u.kind == BookUpdate::Kind::Delta) b.view.evidence.last_authoritative_snapshot_ns = previous_authoritative;
    if (u.evidence.authoritative_snapshot) b.view.evidence.last_authoritative_snapshot_ns = u.evidence.recv_monotonic_ns;
    b.view.evidence.seq_gap = false; b.view.evidence.halted = false;
    auto& d = b.view.depth;
    d.usable_for_depth = true; d.book_version = version; d.last_ws_recv_ns = u.evidence.recv_monotonic_ns;
    for (auto [p, q] : b.bids) state_bytes += field("B") + field(std::to_string(p)) + field(std::to_string(q));
    for (auto [p, q] : b.asks) state_bytes += field("A") + field(std::to_string(p)) + field(std::to_string(q));
    d.snapshot_version_hash = compact_hash(state_bytes);
    for (auto it = b.bids.rbegin(); it != b.bids.rend(); ++it) {
        if(d.bid_count==state::kMaxSnapshotDepth)break;
        b.view.bid_qty_micro[d.bid_count] = u.fractional_quantities ? it->second : multiply(it->second,unit);
        d.bids[d.bid_count++] = {it->first,0.0,static_cast<double>(it->second)/(u.fractional_quantities ? unit : 1)};
    }
    for (auto [p, q] : b.asks) {
        if(d.ask_count==state::kMaxSnapshotDepth)break;
        b.view.ask_qty_micro[d.ask_count] = u.fractional_quantities ? q : multiply(q,unit);
        d.asks[d.ask_count++] = {p,0.0,static_cast<double>(q)/(u.fractional_quantities ? unit : 1)};
    }
    b.view.evidence.last_book_change_ns = changed ? u.evidence.recv_monotonic_ns : previous_change;
    std::string content;
    for(auto [p,q]:b.bids)content+=field("B")+field(std::to_string(p))+field(std::to_string(q));
    for(auto [p,q]:b.asks)content+=field("A")+field(std::to_string(p))+field(std::to_string(q));
    b.view.evidence.content_hash=compact_hash(content);
    state::build_depth_prefix(d.bids, d.bid_count, d.asks, d.ask_count, &d.prefix);
    return true;
}
VersionedBook SynchronizedBookStore::read(const VenueInstrumentKey& key) const {
    const auto it = books_.find(key.identity());
    return it == books_.end() ? VersionedBook{.key = key} : it->second.view;
}
std::array<VersionedBook, 2> SynchronizedBookStore::read_pair(const ApprovedPair& p) const { return {read(p.leg_yes), read(p.leg_no)}; }
void SynchronizedBookStore::note_liveness(const VenueInstrumentKey& key,std::uint64_t now) {
    auto it=books_.find(key.identity());if(it!=books_.end()) it->second.view.evidence.last_liveness_ns=now;
}
void SynchronizedBookStore::invalidate(const VenueInstrumentKey& key,std::string reason,std::uint64_t now) {
    auto& b=books_[key.identity()];b.view.key=key;b.view.depth.usable_for_depth=false;b.view.depth.recovering=true;
    b.view.evidence.seq_gap=true;b.view.evidence.health_reason=std::move(reason);b.last_packet_recv=std::max(b.last_packet_recv,now);
}
bool SynchronizedBookStore::reconcile(const VenueInstrumentKey& key,const VersionedBook& a,std::uint64_t now) {
    auto it=books_.find(key.identity());if(it==books_.end())return false;
    const auto& b=it->second.view;
    bool equal=b.depth.usable_for_depth && a.depth.usable_for_depth && b.depth.bid_count==a.depth.bid_count && b.depth.ask_count==a.depth.ask_count;
    for(std::uint16_t i=0;equal && i<b.depth.bid_count;++i) equal=b.depth.bids[i].price_tick==a.depth.bids[i].price_tick && b.bid_qty_micro[i]==a.bid_qty_micro[i];
    for(std::uint16_t i=0;equal && i<b.depth.ask_count;++i) equal=b.depth.asks[i].price_tick==a.depth.asks[i].price_tick && b.ask_qty_micro[i]==a.ask_qty_micro[i];
    if(!equal){invalidate(key,"AUTHORITATIVE_SNAPSHOT_MISMATCH",now);return false;}
    it->second.view.evidence.last_authoritative_snapshot_ns=now;return true;
}
void validate_books(const ApprovedPair& p, const std::array<VersionedBook, 2>& b, const PairPolicy& policy, std::uint64_t now) {
    validate_pair(p);
    require(b[0].key == p.leg_yes && b[1].key == p.leg_no, "book key mismatch");
    if(policy.asof_enabled) {
        require(!policy.asof.research_only,"RESEARCH_POLICY_NOT_STRICT_ELIGIBLE");
        const auto joint=joint_book_asof(b,now,policy.asof,{true,true},{true,true});
        require(joint.diagnostic_eligible,joint.reason.c_str());return;
    }
    for (const auto& v : b) {
        const auto& e = v.evidence;
        require(v.depth.usable_for_depth && !v.depth.recovering && !v.depth.crossed && !v.depth.closed && !v.depth.resolved &&
                !e.seq_gap && !e.stale && !e.halted && e.snapshot_version > 0, "invalid book quality");
        require(v.depth.book_version == e.snapshot_version && v.depth.last_ws_recv_ns == e.recv_monotonic_ns &&
                v.depth.snapshot_version_hash != 0, "inconsistent depth evidence");
        if (policy.max_liveness_age_ns && policy.max_authoritative_age_ns) {
            require(e.last_liveness_ns && now>=e.last_liveness_ns && now-e.last_liveness_ns<=policy.max_liveness_age_ns,"CONNECTION_NOT_LIVE");
            require(e.last_authoritative_snapshot_ns && now>=e.last_authoritative_snapshot_ns && now-e.last_authoritative_snapshot_ns<=policy.max_authoritative_age_ns,"AUTHORITATIVE_STATE_TOO_OLD");
        } else require(now >= e.recv_monotonic_ns && now - e.recv_monotonic_ns <= policy.max_age_ns, "stale/future book");
    }
    auto a = policy.max_authoritative_age_ns ? b[0].evidence.last_authoritative_snapshot_ns : b[0].evidence.recv_monotonic_ns;
    auto c = policy.max_authoritative_age_ns ? b[1].evidence.last_authoritative_snapshot_ns : b[1].evidence.recv_monotonic_ns;
    require((a > c ? a - c : c - a) <= policy.max_skew_ns, "receive skew exceeds policy");
}
}  // namespace trading_engine::cross_venue
