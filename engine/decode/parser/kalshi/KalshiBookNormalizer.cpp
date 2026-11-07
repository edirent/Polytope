#include "engine/decode/parser/kalshi/KalshiBookNormalizer.h"
#include <boost/json.hpp>
#include <limits>

namespace trading_engine::cross_venue {
namespace {
std::string str(const boost::json::value& v) { return std::string(v.as_string()); }
std::uint64_t uint(const boost::json::value& v) {
    if (v.is_uint64()) return v.as_uint64();
    require(v.is_int64() && v.as_int64() > 0, "invalid WS sequence/session"); return static_cast<std::uint64_t>(v.as_int64());
}
void load_side(const boost::json::array& values, std::map<Money, Money>& out,bool fractional) {
    out.clear();
    for (const auto& v : values) {
        const auto& row = v.as_array(); require(row.size() == 2, "invalid Kalshi level");
        auto price = money(str(row[0])), quantity = money(str(row[1]));
        require(price > 0 && price < unit && quantity >= 0 && quantity <= multiply(max_quantity, unit), "invalid fixed-point level");
        require(out.emplace(price, quantity).second, "duplicate price level");
    }
    require(out.size() <= (fractional?10000:state::kMaxSnapshotDepth), "Kalshi book exceeds V0 depth bound");
}
BookUpdate snapshot(const VenueInstrumentKey& key, bool yes, BookProvenance e,
    const std::map<Money, Money>& y, const std::map<Money, Money>& n, bool fractional) {
    BookUpdate u; u.key = key; u.evidence = e;
    u.fractional_quantities=fractional;u.bounded_prefix=fractional;
    for (auto [p, q] : (yes ? y : n)) u.bids.emplace_back(p, fractional ? q : q / unit);
    for (auto [p, q] : (yes ? n : y)) u.asks.emplace_back(unit - p, fractional ? q : q / unit);
    return u;
}
}
KalshiBookNormalizer::KalshiBookNormalizer(VenueInstrumentKey key, bool yes, bool fractional) : key_(std::move(key)), buy_yes_(yes),fractional_(fractional) {
    require(key_.venue == VenueId::Kalshi, "wrong Kalshi venue");
}
BookUpdate KalshiBookNormalizer::disconnect(BookProvenance p) {
    ready_ = false; p.seq_gap = true; p.raw_hash = sha256("DISCONNECT");
    BookUpdate u; u.key = key_; u.kind = BookUpdate::Kind::Disconnect; u.evidence = p; return u;
}
BookUpdate KalshiBookNormalizer::rest_snapshot(std::string_view raw, BookProvenance p) {
    // REST has no WS subscription sequence. Receiving REST also ends WS readiness.
    ready_ = false;
    try {
        const auto root = boost::json::parse(raw).as_object(); const auto& book = root.at("orderbook_fp").as_object();
        std::map<Money, Money> y, n;
        load_side(book.at("yes_dollars").as_array(), y,fractional_); load_side(book.at("no_dollars").as_array(), n,fractional_);
        p.raw_hash = sha256(raw); p.sequence = 1;
        return snapshot(key_, buy_yes_, p, y, n, fractional_);
    } catch (const std::exception&) { auto u = disconnect(p); u.evidence.raw_hash = sha256(raw); return u; }
}
BookUpdate KalshiBookNormalizer::websocket(std::string_view raw, BookProvenance p, bool subscription_validated) {
    try {
        const auto root = boost::json::parse(raw).as_object(); const auto& msg = root.at("msg").as_object();
        require(str(msg.at("market_ticker")) == key_.market_id, "unmapped Kalshi market");
        const auto seq = uint(root.at("seq")), sid = uint(root.at("sid"));
        require(seq > 0 && sid > 0, "zero WS sequence/session");
        p.raw_hash = sha256(raw); p.sequence = seq;
        p.stream_sid=sid;
        if(const auto* clock=msg.if_contains("clock");clock && (clock->is_uint64() || clock->is_int64())) p.exchange_clock=uint(*clock);
        const auto type = str(root.at("type"));
        if (type == "orderbook_snapshot") {
            std::map<Money, Money> y, n;
            if(msg.contains("yes_dollars_fp") || !fractional_)load_side(msg.at("yes_dollars_fp").as_array(), y,fractional_);
            if(msg.contains("no_dollars_fp") || !fractional_)load_side(msg.at("no_dollars_fp").as_array(), n,fractional_);
            yes_bids_ = std::move(y); no_bids_ = std::move(n); sid_ = sid; seq_ = seq; last_clock_=p.exchange_clock;ready_ = true;
            return snapshot(key_, buy_yes_, p, yes_bids_, no_bids_, fractional_);
        }
        require(type == "orderbook_delta" && ready_ && sid == sid_ && seq_ < std::numeric_limits<std::uint64_t>::max() &&
                (subscription_validated ? seq>seq_ : seq == seq_ + 1),
                "Kalshi delta before snapshot/gap/session switch");
        require(!last_clock_ || !p.exchange_clock || p.exchange_clock>=last_clock_,"Kalshi exchange clock regression requires fresh snapshot");
        const auto side = str(msg.at("side")); require(side == "yes" || side == "no", "invalid Kalshi side");
        auto price = money(str(msg.at("price_dollars"))), delta = money(str(msg.at("delta_fp")));
        require(price > 0 && price < unit, "invalid Kalshi price");
        auto& levels = side == "yes" ? yes_bids_ : no_bids_;
        const auto it = levels.find(price); const Money prior = it == levels.end() ? 0 : it->second;
        const auto quantity = add(prior, delta);
        require(quantity >= 0 && quantity <= multiply(max_quantity, unit), "negative/oversized resulting level");
        if (quantity) levels[price] = quantity; else levels.erase(price);
        require(levels.size() <= (fractional_?10000:state::kMaxSnapshotDepth), "Kalshi book exceeds V0 bound");
        BookUpdate u; u.key = key_; u.kind = BookUpdate::Kind::Delta; u.evidence = p;
        u.fractional_quantities=fractional_;u.bounded_prefix=fractional_;u.subscription_sequence_validated=subscription_validated;
        const bool is_bid = (side == "yes") == buy_yes_;
        (is_bid ? u.bids : u.asks).emplace_back(is_bid ? price : unit - price, fractional_ ? quantity : quantity / unit);
        seq_ = seq;if(p.exchange_clock)last_clock_=p.exchange_clock;return u;
    } catch (const std::exception&) { auto u = disconnect(p); u.evidence.raw_hash = sha256(raw); return u; }
}
}  // namespace trading_engine::cross_venue
