#include "engine/strategy/cross_venue/LiveResearch.h"
#include "engine/state/cross_venue/SynchronizedBookStore.h"
#include "engine/decode/parser/kalshi/KalshiBookNormalizer.h"
#include "engine/decode/parser/polymarket/CrossVenueClobNormalizer.h"
#include "engine/common/math/FractionalVwap.h"
#include <fstream>
#include <iostream>
#include <numeric>
namespace c=trading_engine::cross_venue;namespace r=c::research;
r::Object parse_book(const r::Object& input,c::VersionedBook* output=nullptr){
    const auto venue=r::get_text(input,"venue");const auto market=r::get_text(input,"market_id"),instrument=r::get_text(input,"instrument_id");
    c::VenueInstrumentKey key{c::venue_id(venue),market,instrument,venue=="kalshi"?"USD":"UNKNOWN_POLY_COLLATERAL"};
    c::BookProvenance evidence;evidence.recv_wall_ns=r::get_uint(input,"received_wall_ns");evidence.recv_monotonic_ns=r::get_uint(input,"received_monotonic_ns");evidence.stale=false;evidence.authoritative_snapshot=true;evidence.last_liveness_ns=evidence.last_authoritative_snapshot_ns=evidence.recv_monotonic_ns;
    const auto raw=r::get_text(input,"raw_payload");c::BookUpdate update;
    if(venue=="kalshi"){c::KalshiBookNormalizer parser(key,instrument=="YES",true);update=parser.rest_snapshot(raw,evidence);}
    else {c::CrossVenueClobNormalizer parser(key,true);update=parser.normalize(raw,evidence,1);}
    c::SynchronizedBookStore store(true,true);const bool valid=store.apply(update);const auto book=store.read(key);
    r::Array asks,bids;for(unsigned i=0;i<book.depth.ask_count;++i)asks.push_back(r::Object{{"price_micro",book.depth.asks[i].price_tick},{"quantity_micro",book.ask_qty_micro[i]}});for(unsigned i=0;i<book.depth.bid_count;++i)bids.push_back(r::Object{{"price_micro",book.depth.bids[i].price_tick},{"quantity_micro",book.bid_qty_micro[i]}});
    r::Object row{{"request_id",r::get_text(input,"request_id")},{"venue",venue},{"market_id",market},{"instrument_id",instrument},{"valid",valid},{"evidence_level","ACTUAL_REST_ORDER_BOOK_NOT_CONTINUOUS_SYNCHRONIZED_L2"},{"raw_payload_sha256",c::sha256(raw)},{"received_wall_ns",evidence.recv_wall_ns},{"received_monotonic_ns",evidence.recv_monotonic_ns},{"health_reason",book.evidence.health_reason},{"bids",bids},{"asks",asks},{"bid",valid && book.depth.bid_count?r::json::value(c::decimal(book.depth.bids[0].price_tick)):r::json::value(nullptr)},{"ask",valid && book.depth.ask_count?r::json::value(c::decimal(book.depth.asks[0].price_tick)):r::json::value(nullptr)},{"bid_size",valid && book.depth.bid_count?r::json::value(c::decimal(book.bid_qty_micro[0])):r::json::value(nullptr)},{"ask_size",valid && book.depth.ask_count?r::json::value(c::decimal(book.ask_qty_micro[0])):r::json::value(nullptr)},{"buyable",valid && book.depth.ask_count && book.ask_qty_micro[0]>0},{"retained_best_levels",64},{"live_orders_sent",0}};
    if(output)*output=book;return row;
}
int main(int argc,char** argv){try{
    std::string phase;std::filesystem::path input,out;
    for(int i=1;i<argc;++i){std::string flag=argv[i];c::require(i+1<argc,"missing argument");const std::string value=argv[++i];if(flag=="--phase")phase=value;else if(flag=="--input")input=value;else if(flag=="--out")out=value;else throw std::invalid_argument("unsupported option; read-only inventory has no order submission");}
    c::require(!input.empty() && !out.empty(),"input and output required");c::require(!std::filesystem::exists(out),"refuse to overwrite immutable inventory output");std::ifstream stream(input);c::require(stream.good(),"input unavailable");std::ofstream target(out);target.exceptions(std::ios::badbit|std::ios::failbit);std::string line;std::uint64_t count=0;
    while(std::getline(stream,line)){auto o=r::json::parse(line).as_object();r::Object row;
        try{
            if(phase=="normalize"){auto raw=o.at("raw").as_object();raw["_observed_wall_ns"]=r::get_uint(o.at("source").as_object(),"received_wall_ns");raw["_directional_binary_aliases"]=true;auto market=r::normalize_market(r::get_text(o,"venue"),raw);row=r::market_json(market);row.erase("raw");row["input_id"]=r::get_text(o,"id");}
            else if(phase=="books")row=parse_book(o);
            else if(phase=="economics"){
                c::VersionedBook a,b;auto qa=parse_book(o.at("leg_a").as_object(),&a),qb=parse_book(o.at("leg_b").as_object(),&b);row={{"pair_id",r::get_text(o,"pair_id")},{"direction",r::get_uint(o,"direction")},{"source","ACTUAL_REST_UNAPPROVED_CONDITIONAL_DIAGNOSTIC"},{"verified_live_realized_pnl",nullptr},{"strict_pnl",nullptr},{"live_orders_sent",0}};r::Array quantities;
                auto ma=r::normalize_market(r::get_text(o.at("market_a").as_object(),"venue"),o.at("market_a").as_object().at("raw").as_object());auto mb=r::normalize_market(r::get_text(o.at("market_b").as_object(),"venue"),o.at("market_b").as_object().at("raw").as_object());
                for(const auto& value:o.at("quantities_micro").as_array()){const auto q=value.is_int64()?value.as_int64():static_cast<std::int64_t>(value.as_uint64());auto ca=trading_engine::common::math::buy_vwap_micro(a,q),cb=trading_engine::common::math::buy_vwap_micro(b,q);r::Object v{{"quantity_micro",q},{"depth_supported",ca.ok && cb.ok},{"gross",nullptr},{"fees",nullptr},{"budget_net",nullptr},{"fee_net",nullptr}};
                    if(ca.ok && cb.ok){const auto cost=c::add(ca.cost,cb.cost),gross=c::add(q,-cost);v["cost_a"]=c::decimal(ca.cost);v["cost_b"]=c::decimal(cb.cost);v["gross"]=c::decimal(gross);try{const auto fee=c::add(r::estimate_fee_micro(r::fee_schedule(ma),a,q,r::get_uint(o,"decision_wall_ns")),r::estimate_fee_micro(r::fee_schedule(mb),b,q,r::get_uint(o,"decision_wall_ns")));const auto budget=static_cast<c::Money>((static_cast<__int128>(q)*10000+c::unit-1)/c::unit);v["fees"]=c::decimal(fee);v["fee_net"]=c::decimal(gross-fee);v["budget_net"]=c::decimal(gross-fee-budget);}catch(const std::exception& e){v["fee_status"]=std::string(e.what());}}
                    quantities.push_back(v);
                }row["sizes"]=quantities;
            }else throw std::invalid_argument("unsupported inventory phase");
        }catch(const std::exception& e){row={{"input_id",r::get_text(o,"id")},{"request_id",r::get_text(o,"request_id")},{"pair_id",r::get_text(o,"pair_id")},{"valid",false},{"error",std::string(e.what())},{"retained_not_silently_excluded",true},{"live_orders_sent",0}};}
        target<<r::json::serialize(row)<<'\n';++count;
    }
    std::cout<<count<<" read-only native inventory records processed\n";return 0;
}catch(const std::exception& e){std::cerr<<"Inventory stopped: "<<e.what()<<'\n';return 1;}}
