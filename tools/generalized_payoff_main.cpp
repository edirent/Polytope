#include "engine/strategy/cross_venue/GeneralizedResearch.h"
#include <iostream>
namespace r=trading_engine::cross_venue::research;
int main(int argc,char** argv){try{std::string phase="discover";std::filesystem::path config="config/cross_venue_v0_1_3.json",input,relationships,run,out;
    for(int i=1;i<argc;++i){std::string a=argv[i];if(a=="--help"){std::cout<<"Read-only generalized payoff research: --phase discover|replay --config FILE --universe metadata.json --relationships FILE --run-dir CAPTURE --out EMPTY_DIR\n";return 0;}trading_engine::cross_venue::require(i+1<argc,"missing option value");std::string v=argv[++i];if(a=="--phase")phase=v;else if(a=="--config")config=v;else if(a=="--universe")input=v;else if(a=="--relationships")relationships=v;else if(a=="--run-dir")run=v;else if(a=="--out")out=v;else throw std::invalid_argument("unsupported option; no order submission exists");}
    trading_engine::cross_venue::require(!out.empty(),"--out required");auto c=r::load_config(config);r::Object result;
    if(phase=="discover")result=r::generalized_discover(c,input,out);else if(phase=="replay")result=r::generalized_replay(c,relationships,run,out);else throw std::invalid_argument("unknown phase");std::cout<<r::json::serialize(result)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"Generalized research stopped: "<<e.what()<<'\n';return 1;}}
