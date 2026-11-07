#include "engine/strategy/cross_venue/LiveResearch.h"
#include <iostream>
#include <sstream>
#include <cstdlib>

namespace cv=trading_engine::cross_venue;
namespace r=cv::research;
int main(int argc,char** argv){
    try{
        std::filesystem::path config="config/cross_venue_v0_1.json",directory,output;
        unsigned duration=600;cv::Money capital=200000*cv::unit;std::vector<unsigned> latencies{0,100,250,500,1000};
        for(int i=1;i<argc;++i){const std::string option=argv[i];
            if(option=="--help"){std::cout<<CV_RESEARCH_COMMAND<<" --config FILE [--run-dir DIR] [--duration-sec 600] [--capital 200000] [--latencies-ms 0,100,250,500,1000] [--out EMPTY_DIR]\nRead-only public market data and simulated execution only. No trading endpoints.\n";return 0;}
            cv::require(i+1<argc,"option requires value");const std::string value=argv[++i];
            if(option=="--config")config=value;else if(option=="--run-dir")directory=value;else if(option=="--out")output=value;
            else if(option=="--duration-sec")duration=static_cast<unsigned>(std::stoul(value));else if(option=="--capital")capital=cv::money(value);
            else if(option=="--latencies-ms"){latencies.clear();std::istringstream in(value);std::string item;while(std::getline(in,item,',')){const auto n=std::stoul(item);cv::require(n<=60000,"latency too large");latencies.push_back(n);}}
            else throw std::invalid_argument("unsupported option; no live-order flag exists");
        }
        const std::string command=CV_RESEARCH_COMMAND;
        if(command=="cross_venue_key_check"){
            const auto path=std::getenv("KALSHI_PRIVATE_KEY_PATH");
            std::cout<<r::json::serialize(r::inspect_kalshi_signing_key(path && *path?path:"/home/edirent/.secrets/kalshi.pem"))<<'\n';
        }else if(command=="cross_venue_discover"){
            const auto c=r::load_config(config);if(directory.empty())directory=c.runs_root/("discovery_"+std::to_string(r::wall_ns()));
            cv::require(!std::filesystem::exists(directory)||std::filesystem::is_empty(directory),"discovery output must be new/empty");
            const auto result=r::discover(c,directory);r::Array rejections;const auto approved=r::load_approved_pairs(c,result.markets,rejections);
            r::write_json(directory/"discovery_manifest.json",r::Object{{"evidence_level","REAL_PUBLIC_METADATA"},{"markets",result.markets.size()},
                {"candidate_pairs",result.candidates.size()},{"approved_pairs",approved.size()},{"approval_rejections",rejections},
                {"requests",result.requests},{"verified_live_realized_pnl",nullptr}});
            std::cout<<"Discovered "<<result.markets.size()<<" markets; candidate pairs="<<result.candidates.size()<<"; approved="<<approved.size()<<"; output="<<directory<<'\n';
        }else if(command=="run_cross_venue_live_shadow"){
            const auto result=r::capture(r::load_config(config),duration,directory);
            const auto summary=r::replay(result.directory,capital,latencies,output);
            std::cout<<"Verdict: "<<r::get_text(summary,"verdict")<<"; verification="<<r::get_text(summary,"verification_status")<<'\n';
        }else if(command=="cross_venue_economic_discovery"){
            cv::require(!directory.empty() && !output.empty(),"--run-dir and --out required");
            std::cout<<r::json::serialize(r::economic_discovery(directory,output))<<'\n';
        }else if(command=="cross_venue_feed_audit"){
            cv::require(!directory.empty() && !output.empty(),"--run-dir and --out are required");
            std::cout<<r::json::serialize(r::audit_feeds(directory,output))<<'\n';
        }else{
            cv::require(!directory.empty(),"--run-dir is required");const auto summary=r::replay(directory,capital,latencies,output);
            std::cout<<r::json::serialize(summary)<<'\n';
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<CV_RESEARCH_COMMAND<<": "<<e.what()<<'\n';return 1;}
}
