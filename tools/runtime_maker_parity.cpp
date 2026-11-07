// Golden comparison uses the unchanged MarketMakingEngine used by legacy maker runners.
// Frozen immutable frames and identical OMS feedback are the comparison boundary.
#include "engine/runtime/StrategyArtifacts.h"
#include "engine/runtime/DurableJournal.h"
#include "engine/signal/reader/OracleArtifactReader.h"
#include <fstream>
#include <map>
#include <iostream>
namespace j=boost::json;namespace rt=trading_engine::runtime;namespace mm=trading_engine::strategy::market_making;
int main(int argc,char** argv) {
    try {
        if(argc==3 && std::string(argv[1])=="--inspect-artifact") {
            trading_engine::signal::OracleArtifactReader artifact;auto loaded=artifact.load(argv[2]);if(!loaded.ok)throw std::runtime_error("artifact load failed");
            std::map<std::string,std::uint32_t> indices;for(const auto& plan:artifact.active_runtime_plans())for(std::uint16_t n=0;n<plan.leg_count;++n)indices[*plan.asset_ids[n]]=plan.asset_indices[n];
            j::object out;for(const auto& [asset,index]:indices)out[asset]=index;std::cout<<j::serialize(out)<<'\n';return 0;
        }
        if(argc!=4)throw std::runtime_error("usage: runtime_maker_parity CONFIG EVENTS OUTPUT_CSV");
        std::ifstream config(argv[1]);std::string bytes{std::istreambuf_iterator<char>(config),{}};auto cfg=j::parse(bytes).as_object();
        std::map<std::string,std::unique_ptr<mm::MarketMakingEngine>> old;
        for(const auto& v:cfg.at("strategies").as_array()) {
            const auto& i=v.as_object();if(i.at("strategy_id").as_string()!="market_making")continue;
            std::string id(i.at("strategy_instance_id").as_string());auto model=i.at("model").as_object();
            old[id]=std::make_unique<mm::MarketMakingEngine>(rt::maker_model_config(model,id,rt::sha256(j::serialize(model)),cfg.at("max_book_age_ns").to_number<std::uint64_t>()));
        }
        std::ifstream input(argv[2]);std::ofstream output(argv[3]);output<<"strategy_instance_id,availability_ns,asset_id,fair_value_tick,quotes,cancels,result_hash_equal,quotes_equal,cancels_equal\n";
        std::string line;std::size_t frames=0,differences=0;
        while(std::getline(input,line)) {
            auto envelope=j::parse(line).as_object();auto& e=envelope.at("event").as_object();auto type=std::string(e.at("type").as_string());
            if(type=="STRATEGY_DESIRED_RESET") {auto owner=std::string(e.at("strategy_instance_id").as_string());old.at(owner)->remove_active_quote(e.at("asset_index").to_number<std::uint32_t>());}
            if(type!="STRATEGY_FRAME")continue;
            auto owner=std::string(e.at("strategy_instance_id").as_string());auto raw_frame=e.at("frame").as_object();
            // The frame reader's other quantities are signed. Provenance hashes span all uint64 values.
            const auto full_hash=raw_frame.at("snapshot_version_hash").to_number<std::uint64_t>();raw_frame["snapshot_version_hash"]=0;
            auto frame=rt::maker_frame_from_json(raw_frame);frame.depth.snapshot_version_hash=full_hash;
            auto now=e.at("frame").as_object().at("now_ns").to_number<std::uint64_t>();
            mm::MarketMakingInput in;in.market_id=frame.market_id;in.asset_id=frame.asset_id;in.market_index=frame.market_index;in.asset_index=frame.asset_index;
            in.depth=&frame.depth;in.current_position_lots=frame.position_lots;in.now_ns=now;
            auto result=old.at(owner)->on_market_update(in);j::array quotes,cancels;
            for(std::uint16_t n=0;n<result.quote_count;++n)quotes.push_back(rt::quote_payload_json(result.quotes[n]));
            for(std::uint16_t n=0;n<result.cancel_count;++n)cancels.push_back(result.cancels[n].idempotency_hash);
            const bool hash=result.output_hash==e.at("result_hash").to_number<std::uint64_t>(),q=quotes==e.at("quotes").as_array(),c=cancels==e.at("cancel_hashes").as_array();
            ++frames;if(!hash || !q || !c)++differences;
            output<<owner<<','<<now<<','<<frame.asset_id<<','<<result.fair_value_tick<<','<<result.quote_count<<','<<result.cancel_count<<','<<hash<<','<<q<<','<<c<<'\n';
        }
        std::cout<<j::serialize(j::object{{"frames",frames},{"differences",differences},{"scope","Existing generic maker runner core, identical inputs/config/feedback; product-runner orchestration not certified"}})<<'\n';
        return frames && !differences?0:2;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
