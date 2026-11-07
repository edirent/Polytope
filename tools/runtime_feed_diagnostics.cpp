// Offline forensic driver over the SAME existing sequence-aware state implementation.
// No strategy scan, account mutation, authentication or network client is invoked.
#include "engine/strategy/cross_venue/LiveResearch.h"
#include <fstream>
#include <deque>
#include <set>
#include <iostream>
#include "engine/state/cross_venue/JointBookAsOf.h"
#include <map>
namespace r=trading_engine::cross_venue::research;
int main(int argc,char** argv) {
    try {
        if(argc!=4)throw std::runtime_error("usage: diagnostics CONFIG RAW_JOURNAL OUTPUT_JSONL");
        auto config=r::read_object(argv[1]);std::ifstream metadata(r::get_text(config,"markets_file"));
        std::string bytes{std::istreambuf_iterator<char>(metadata),{}};auto array=r::json::parse(bytes).as_array();
        std::vector<r::Market> markets;for(auto& value:array){auto& row=value.as_object();markets.push_back(r::normalize_market(r::get_text(row,"venue"),row.at("raw").as_object()));}
        r::Config cfg;cfg.policy.asof_enabled=true;cfg.original={{"reconciliation_policy","quiet_request_v0.1.1"}};
        cfg.heartbeat_ms=30000;cfg.authoritative_ms=r::get_uint(config,"max_book_age_ns")/1000000;
        r::ResearchState state(cfg,markets,{});std::set<std::string> recent;std::deque<std::string> order;
        std::ifstream input(argv[2]);std::ofstream output(argv[3]);output.exceptions(std::ios::failbit|std::ios::badbit);
        std::ofstream admission(std::string(argv[3])+".admission.jsonl");
        trading_engine::cross_venue::JointBookPolicy policy;
        policy.liveness_ns=30000000000ULL;policy.authority_ns=r::get_uint(config,"max_book_age_ns");policy.change_ns=policy.authority_ns;
        std::map<std::string,bool> previous;std::map<std::string,bool> initialized;
        auto track=[&](std::uint64_t now,std::uint64_t input_index,const std::string& trigger) {
            for(const auto& market:markets) {
                const auto& current=state.market(market.yes);
                auto joint=trading_engine::cross_venue::joint_book_asof(
                    {state.books().read(market.yes),state.books().read(market.no)},now,policy,
                    {state.websocket_book(market.yes),state.websocket_book(market.no)},{current.tradable,current.tradable},false);
                const auto key=market.venue+":"+market.id;
                if(!initialized[key] || previous[key]!=joint.diagnostic_eligible) {
                    initialized[key]=true;previous[key]=joint.diagnostic_eligible;
                    admission<<r::json::serialize(r::Object{{"input_index",input_index},{"availability_ns",now},{"venue",market.venue},
                        {"market_id",market.id},{"admitted",joint.diagnostic_eligible},{"reason",joint.reason},{"trigger",trigger}})<<'\n';
                }
            }
        };
        std::string line;std::uint64_t index=0;
        while(std::getline(input,line)) {
            if(input.eof())break;auto envelope=r::json::parse(line).as_object();auto row=envelope.at("event").as_object();
            auto current=index++;if(r::get_text(row,"kind")!="venue_packet") {track(r::get_uint(row,"recv_ns"),current,"timer");continue;}
            row["recv_monotonic_ns"]=r::get_uint(row,"callback_recv_monotonic_ns",r::get_uint(row,"recv_ns"));
            row["availability_monotonic_ns"]=r::get_uint(row,"recv_ns");row["raw_hash"]=trading_engine::cross_venue::sha256(r::get_text(row,"raw_payload"));
            const auto venue=r::get_text(row,"venue"),transport=r::get_text(row,"transport"),classification=r::get_text(row,"classification");
            if(transport=="ws" && classification=="market_data") {
                const auto id=venue+":"+std::to_string(r::get_uint(row,"connection_epoch"))+":"+r::get_text(row,"raw_hash");
                if(!recent.insert(id).second)row["classification"]="heartbeat";
                else {order.push_back(id);if(order.size()>4096){recent.erase(order.front());order.pop_front();}}
            }
            if(transport=="event" && (classification=="connection_error" || classification=="authentication_error"))row["classification"]="disconnect";
            (void)state.process(row);
            track(r::get_uint(row,"recv_ns"),current,classification);
            for(auto& v:state.take_diagnostics()) {
                auto detail=v.as_object();detail["input_index"]=current;detail["source_classification"]=classification;output<<r::json::serialize(detail)<<'\n';
            }
        }
        std::cout<<r::json::serialize(state.counters())<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
