#include "engine/state/cross_venue/JointBookAsOf.h"
#include <algorithm>
namespace trading_engine::cross_venue {
JointBookAsOf joint_book_asof(const std::array<VersionedBook,2>& b,std::uint64_t t,
    const JointBookPolicy& p,std::array<bool,2> ws,std::array<bool,2> tradable,bool approved) {
    require(p.liveness_ns && p.authority_ns && p.change_ns,"invalid As-Of thresholds");
    JointBookAsOf j;j.decision_time=t;
    auto age_ok=[&](std::uint64_t at,std::uint64_t limit){return at && at<=t && t-at<=limit;};
    for(unsigned i=0;i<2;++i){const auto& v=b[i];const auto& e=v.evidence;
        j.available &= e.recv_monotonic_ns && e.recv_monotonic_ns<=t && e.last_authoritative_snapshot_ns<=t && e.last_liveness_ns<=t && e.last_book_change_ns<=t;
        j.transport_healthy &= ws[i] && age_ok(e.last_liveness_ns,p.liveness_ns);
        j.reconstruction_valid &= v.depth.usable_for_depth && !v.depth.recovering && !v.depth.crossed && !e.seq_gap && !e.stale && !e.halted && e.snapshot_version && v.depth.book_version==e.snapshot_version && v.depth.snapshot_version_hash && v.depth.last_ws_recv_ns==e.recv_monotonic_ns;
        const bool authority=age_ok(e.last_authoritative_snapshot_ns,p.authority_ns);
        j.source_authority_valid &= authority;
        // A recently validated quiet book may outlive its last actual change.
        j.book_freshness_acceptable &= age_ok(e.last_book_change_ns,p.change_ns) || (authority && e.last_authoritative_snapshot_ns>=e.last_book_change_ns);
        j.executable_asks_available &= v.depth.ask_count>0;
        j.both_markets_tradable &= tradable[i] && !v.depth.closed && !v.depth.resolved && !e.halted;
    }
    const auto a=b[0].evidence.recv_monotonic_ns,c=b[1].evidence.recv_monotonic_ns;
    j.update_skew_ns=a>c?a-c:c-a;
    if(!j.available)j.reason="MISSING_OR_FUTURE_EVIDENCE";
    else if(!j.reconstruction_valid)j.reason="BOOK_INTEGRITY";
    else if(!j.transport_healthy)j.reason="TRANSPORT_NOT_LIVE";
    else if(!j.both_markets_tradable)j.reason="MARKET_STATUS";
    else if(!j.source_authority_valid)j.reason="STALE_AUTHORITY";
    else if(!j.book_freshness_acceptable)j.reason="STALE_BOOK";
    else if(!j.executable_asks_available)j.reason="NO_EXECUTABLE_ASK";
    else if(p.legacy_skew && j.update_skew_ns>100000000)j.reason="LEGACY_UPDATE_SKEW";
    else j.reason="ELIGIBLE";
    j.diagnostic_eligible=j.reason=="ELIGIBLE";
    j.strict_strategy_eligible=j.diagnostic_eligible && approved && !p.research_only;
    return j;
}
}
