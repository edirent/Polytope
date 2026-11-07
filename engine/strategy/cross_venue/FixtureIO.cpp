#include "engine/strategy/cross_venue/FixtureIO.h"
#include <fstream>
#include <iterator>
#include <set>

namespace trading_engine::cross_venue {
namespace {
std::string str(const boost::json::value& v) { return std::string(v.as_string()); }
std::uint64_t uint(const boost::json::value& v) {
    if (v.is_uint64()) return v.as_uint64();
    require(v.is_int64() && v.as_int64() >= 0, "expected nonnegative integer"); return static_cast<std::uint64_t>(v.as_int64());
}
std::int64_t qty(const boost::json::value& v) {
    const auto n = uint(v); require(n <= static_cast<std::uint64_t>(max_quantity), "quantity too large"); return static_cast<std::int64_t>(n);
}
VenueInstrumentKey key(const boost::json::value& v) {
    const auto& o = v.as_object();
    return {venue_id(str(o.at("venue"))), str(o.at("market_id")), str(o.at("instrument_id")), str(o.at("currency"))};
}
std::array<Money, 2> amounts(const boost::json::value& v) {
    const auto& a = v.as_array(); require(a.size() == 2, "expected two account values");
    return {money(str(a[0])), money(str(a[1]))};
}
boost::json::object root(const std::filesystem::path& path, std::string_view schema) {
    auto o = boost::json::parse(read_bytes(path)).as_object();
    require(str(o.at("schema")) == schema && str(o.at("mode")) == "SYNTHETIC", "only versioned SYNTHETIC fixtures accepted");
    return o;
}
BookUpdate update(const boost::json::object& o) {
    BookUpdate u; u.key = key(o.at("key"));
    const auto kind = str(o.at("kind"));
    if (kind == "snapshot") u.kind = BookUpdate::Kind::Snapshot;
    else if (kind == "delta") u.kind = BookUpdate::Kind::Delta;
    else if (kind == "disconnect") u.kind = BookUpdate::Kind::Disconnect;
    else if (kind == "halt") u.kind = BookUpdate::Kind::Halt;
    else throw std::invalid_argument("unsupported replay event kind");
    u.evidence.recv_monotonic_ns = uint(o.at("recv_monotonic_ns"));
    u.evidence.source_ts_ns = uint(o.at("source_ts_ns")); u.evidence.sequence = uint(o.at("sequence"));
    u.evidence.raw_log_offset = uint(o.at("raw_log_offset")); u.evidence.raw_hash = str(o.at("raw_hash"));
    const auto raw = str(o.at("raw_payload")); require(sha256(raw) == u.evidence.raw_hash, "raw packet checksum mismatch");
    const auto payload = boost::json::parse(raw).as_object();
    if (u.kind == BookUpdate::Kind::Snapshot || u.kind == BookUpdate::Kind::Delta) {
        u.complete = payload.at("complete").as_bool();
        auto levels = [&](const char* name, auto& out) {
            for (const auto& row : payload.at(name).as_array()) {
                const auto& a = row.as_array(); require(a.size() == 2, "invalid price level");
                out.emplace_back(money(str(a[0])), qty(a[1]));
            }
        };
        levels("bids", u.bids); levels("asks", u.asks);
    }
    return u;
}
}  // namespace
std::string read_bytes(const std::filesystem::path& p) {
    require(std::filesystem::is_regular_file(p) && std::filesystem::file_size(p) <= 64 * 1024 * 1024, "input absent/too large");
    std::ifstream stream(p, std::ios::binary); require(stream.good(), "cannot read input");
    return {std::istreambuf_iterator<char>(stream), {}};
}
ApprovedPair parse_pair(const boost::json::object& o, bool require_synthetic) {
    ApprovedPair p;
    p.pair_id = str(o.at("pair_id")); p.leg_yes = key(o.at("leg_yes")); p.leg_no = key(o.at("leg_no"));
    p.rules_hash_yes = str(o.at("rules_hash_yes")); p.rules_hash_no = str(o.at("rules_hash_no")); p.review_id = str(o.at("review_id"));
    p.approval_revision = uint(o.at("approval_revision")); p.enabled = o.at("enabled").as_bool(); p.synthetic = o.at("synthetic").as_bool();
    p.artifact_hash = str(o.at("artifact_hash"));
    for (const auto& s : o.at("payoffs").as_array()) {
        const auto& a = s.as_object(); p.payoffs.push_back({str(a.at("name")), money(str(a.at("yes"))), money(str(a.at("no")))});
    }
    validate_pair(p); require(!require_synthetic || p.synthetic, "fixtures must use synthetic pair approval"); return p;
}
BookFixture load_books(const std::filesystem::path& path) {
    const auto o = root(path, "cross-venue-books-v0"); BookFixture f;
    f.pair = parse_pair(o.at("pair").as_object()); f.starting_cash = amounts(o.at("starting_cash"));
    std::set<std::int64_t> quantities;
    for (const auto& v : o.at("fee_quotes").as_array()) {
        const auto& a = v.as_object(); FeeQuote q;
        q.qty = qty(a.at("qty")); q.fees = amounts(a.at("fees")); q.slippage = amounts(a.at("slippage"));
        q.version = str(a.at("version")); q.pair_artifact_hash = str(a.at("pair_artifact_hash"));
        q.valid_from_ns = uint(a.at("valid_from_ns")); q.valid_until_ns = uint(a.at("valid_until_ns"));
        require(quantities.insert(q.qty).second, "duplicate fee quantity"); f.fees.push_back(q);
    }
    std::set<std::string> names;
    for (const auto& v : o.at("scenarios").as_array()) {
        const auto& a = v.as_object(); BookScenario s;
        s.name = str(a.at("name")); s.now_ns = uint(a.at("now_ns"));
        require(names.insert(s.name).second, "duplicate scenario");
        for (const auto& e : a.at("events").as_array()) {
            auto u = update(e.as_object());
            require(u.key == f.pair.leg_yes || u.key == f.pair.leg_no, "unmapped instrument in replay");
            s.updates.push_back(std::move(u));
        }
        f.scenarios.push_back(std::move(s));
    }
    return f;
}
FillFixture load_fills(const std::filesystem::path& path) {
    const auto o = root(path, "cross-venue-fills-v0"); FillFixture f;
    f.pair = parse_pair(o.at("pair").as_object()); f.starting_cash = amounts(o.at("starting_cash"));
    std::set<std::string> names;
    for (const auto& v : o.at("scenarios").as_array()) {
        const auto& a = v.as_object(); FillScenario s;
        s.name = str(a.at("name")); s.cutoff_ns = uint(a.at("cutoff_ns")); s.planned_quantity = qty(a.at("planned_quantity"));
        require(names.insert(s.name).second, "duplicate scenario");
        if (!a.at("illustrative_outcome").is_null()) {
            const auto outcome = str(a.at("illustrative_outcome")); require(outcome == "YES" || outcome == "NO", "invalid outcome"); s.illustrative_yes = outcome == "YES";
        }
        for (const auto& vfill : a.at("fills").as_array()) {
            const auto& r = vfill.as_object(); FillReceipt receipt;
            receipt.key = key(r.at("key")); receipt.fill_id = str(r.at("fill_id")); receipt.order_id = str(r.at("order_id"));
            receipt.pair_id = str(r.at("pair_id")); const auto payoff = str(r.at("canonical_payoff"));
            require(payoff == "YES" || payoff == "NO", "invalid canonical payoff"); receipt.canonical_yes = payoff == "YES";
            receipt.qty = qty(r.at("qty")); receipt.price = money(str(r.at("price"))); receipt.fee_paid = money(str(r.at("fee_paid")));
            receipt.exchange_time_ns = uint(r.at("exchange_time_ns")); receipt.received_time_ns = uint(r.at("received_time_ns"));
            receipt.raw_log_offset = uint(r.at("raw_log_offset")); receipt.raw_hash = str(r.at("raw_hash"));
            const auto raw = str(r.at("raw_payload")); require(sha256(raw) == receipt.raw_hash, "receipt raw checksum mismatch");
            // Raw receipt is a canonical copy of every field except its own hash/payload.
            auto raw_object = boost::json::parse(raw).as_object(); auto copy = r;
            copy.erase("raw_payload"); copy.erase("raw_hash"); require(raw_object == copy, "receipt raw/envelope mismatch");
            receipt.fee_version = str(r.at("fee_version")); require(str(r.at("mode")) == "SIMULATED", "live receipt in simulation");
            validate_receipt(f.pair, receipt); s.receipts.push_back(std::move(receipt));
        }
        f.scenarios.push_back(std::move(s));
    }
    return f;
}
std::string csv(std::string_view v) {
    std::string out = "\""; for (char c : v) { if (c == '"') out += '"'; out += c; } return out + '"';
}
PairPolicy load_policy(const std::filesystem::path& path) {
    const auto o = boost::json::parse(read_bytes(path)).as_object();
    require(str(o.at("schema")) == "cross-venue-policy-v0" && str(o.at("mode")) == "shadow" && !o.at("live_enabled").as_bool(), "V0 policy must disable live execution");
    PairPolicy p;
    p.max_age_ns = uint(o.at("max_book_age_ns")); p.max_skew_ns = uint(o.at("max_receive_skew_ns"));
    p.max_total_payout = money(str(o.at("max_total_payout"))); p.max_single_leg_loss = money(str(o.at("max_single_leg_loss")));
    p.min_profit = money(str(o.at("min_profit"))); p.min_profit_bps = static_cast<std::int64_t>(uint(o.at("min_profit_bps")));
    p.min_quantity = qty(o.at("min_quantity")); p.quantity_step = qty(o.at("quantity_step"));
    p.quantities.clear(); std::set<std::int64_t> seen;
    for (const auto& v : o.at("quantities").as_array()) {
        const auto n = qty(v); require(n > 0 && seen.insert(n).second, "duplicate/zero candidate quantity"); p.quantities.push_back(n);
    }
    require(!p.quantities.empty() && p.max_total_payout > 0 && p.max_single_leg_loss > 0 && p.min_profit >= 0 &&
        p.min_profit_bps >= 0 && p.min_profit_bps <= 10'000 && p.min_quantity > 0 && p.quantity_step > 0, "invalid policy limits");
    return p;
}
}  // namespace trading_engine::cross_venue
