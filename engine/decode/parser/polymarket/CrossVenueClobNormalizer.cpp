#include "engine/decode/parser/polymarket/CrossVenueClobNormalizer.h"
#include <boost/json.hpp>
#include <limits>

namespace trading_engine::cross_venue {
namespace {
std::string str(const boost::json::value& v) { return std::string(v.as_string()); }
std::int64_t quantity(const boost::json::value& v,bool fractional) {
    const auto n = money(str(v)); require(n >= 0 && n <= multiply(max_quantity, unit), "invalid CLOB quantity"); return fractional ? n : n / unit;
}
Money price(const boost::json::value& v,bool endpoints) { const auto p = money(str(v)); require(endpoints ? p>=0 && p<=unit : p>0 && p<unit, "invalid CLOB price"); return p; }
}
CrossVenueClobNormalizer::CrossVenueClobNormalizer(VenueInstrumentKey key,bool fractional) : key_(std::move(key)),fractional_(fractional) {
    require(key_.venue == VenueId::PolymarketInternational, "CLOB and Polymarket US adapters cannot be mixed");
}
BookUpdate CrossVenueClobNormalizer::disconnect(BookProvenance p) {
    ready_ = false; p.seq_gap = true; p.raw_hash = sha256("DISCONNECT");
    BookUpdate u; u.key = key_; u.kind = BookUpdate::Kind::Disconnect; u.evidence = p; return u;
}
BookUpdate CrossVenueClobNormalizer::normalize(std::string_view raw, BookProvenance p, std::uint64_t local_sequence) {
    try {
        auto root = boost::json::parse(raw).as_object();
        if (const auto* wrapped=root.if_contains("payload");wrapped && wrapped->is_object()) {
            const auto event_type=root.at("type");root=wrapped->as_object();root["event_type"]=event_type;
            auto rename=[](auto& object,const char* from,const char* to){if(auto* v=object.if_contains(from))object[to]=*v;};
            rename(root,"conditionId","market");rename(root,"condition_id","market");rename(root,"assetId","asset_id");rename(root,"priceChanges","price_changes");
            if(auto* changes=root.if_contains("price_changes");changes && changes->is_array())
                for(auto& change:changes->as_array())rename(change.as_object(),"assetId","asset_id");
        }
        require(str(root.at("market")) == key_.market_id, "unmapped CLOB condition");
        // REST book has no event_type. This adapter expects the original CLOB
        // wire shape, not the unified SDK topic/payload wrapper.
        const auto type = root.contains("event_type") ? str(root.at("event_type")) : "book";
        p.raw_hash = sha256(raw); p.sequence = local_sequence;
        require(local_sequence > 0, "zero local packet sequence");
        BookUpdate u; u.key = key_; u.evidence = p;u.fractional_quantities=fractional_;u.bounded_prefix=fractional_;
        if (type == "book") {
            require(str(root.at("asset_id")) == key_.instrument_id, "wrong CLOB token");
            for (const auto& [name, bids] : {std::pair{"bids", true}, std::pair{"asks", false}}) {
                for (const auto& v : root.at(name).as_array()) {
                    const auto& row = v.as_object();
                    (bids ? u.bids : u.asks).emplace_back(price(row.at("price"),fractional_), quantity(row.at("size"),fractional_));
                }
            }
            ready_ = true; seq_ = local_sequence; return u;
        }
        require(type == "price_change" && ready_ && seq_ < std::numeric_limits<std::uint64_t>::max() && local_sequence == seq_ + 1,
                "CLOB update before snapshot/local capture gap");
        u.kind = BookUpdate::Kind::Delta;
        for (const auto& v : root.at("price_changes").as_array()) {
            const auto& row = v.as_object();
            if (str(row.at("asset_id")) != key_.instrument_id) continue;
            const auto side = str(row.at("side")); require(side == "BUY" || side == "SELL", "unknown CLOB side");
            (side == "BUY" ? u.bids : u.asks).emplace_back(price(row.at("price"),fractional_), quantity(row.at("size"),fractional_));
        }
        require(!u.bids.empty() || !u.asks.empty(), "no updates for subscribed token");
        seq_ = local_sequence; return u;
    } catch (const std::exception&) { auto u = disconnect(p); u.evidence.raw_hash = sha256(raw); return u; }
}
}  // namespace trading_engine::cross_venue
