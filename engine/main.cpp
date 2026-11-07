#include "engine/runtime/Runtime.h"
#include <iostream>
#include <string>
int main(int argc,char** argv) {
    try {
        if(argc<3 || std::string(argv[1])!="--config" || argc>4 || (argc==4 && std::string(argv[3])!="--recover"))
            throw std::runtime_error("usage: trading_runtime --config FILE [--recover]");
        return trading_engine::runtime::run(argv[2],argc==4);
    } catch(const std::exception& e) { std::cerr<<"runtime refused: "<<e.what()<<'\n'; return 1; }
}
