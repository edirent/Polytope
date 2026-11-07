#include "oracle/artifact/ApprovedPairArtifact.h"
#include <set>

namespace trading_engine::cross_venue {
std::string pair_hash(const ApprovedPair& p) {
    std::string bytes = field("cross-venue-pair-v0") + field(p.pair_id) + field(p.leg_yes.identity()) + field(p.leg_no.identity())
        + field(p.rules_hash_yes) + field(p.rules_hash_no) + field(p.review_id) + field(std::to_string(p.approval_revision))
        + field(p.enabled ? "1" : "0") + field(p.synthetic ? "1" : "0");
    for (const auto& s : p.payoffs) bytes += field(s.name) + field(std::to_string(s.yes)) + field(std::to_string(s.no));
    return sha256(bytes);
}
void validate_pair(const ApprovedPair& p) {
    require(p.enabled && !p.pair_id.empty() && p.approval_revision > 0 && !p.review_id.empty(), "pair not approved/enabled");
    require(p.leg_yes.venue != p.leg_no.venue, "pair requires different venues");
    require(!p.leg_yes.market_id.empty() && !p.leg_no.market_id.empty() && !p.leg_yes.instrument_id.empty() && !p.leg_no.instrument_id.empty(), "missing instrument");
    require(p.leg_yes.currency == p.leg_no.currency && !p.leg_yes.currency.empty(), "currency mismatch: FX review required");
    require(valid_sha256(p.rules_hash_yes) && valid_sha256(p.rules_hash_no), "missing rule hashes");
    require(p.payoffs.size() >= 2, "missing payoff states");
    std::set<std::string> names;
    bool yes = false, no = false;
    for (const auto& s : p.payoffs) {
        require(!s.name.empty() && names.insert(s.name).second, "duplicate payoff state");
        require((s.yes == unit && s.no == 0) || (s.yes == 0 && s.no == unit), "non-complementary or nonbinary payoff");
        yes |= s.yes == unit; no |= s.no == unit;
    }
    require(yes && no && p.artifact_hash == pair_hash(p), "artifact hash/payoff coverage mismatch");
}
}  // namespace trading_engine::cross_venue
