#pragma once
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <string>
#include <vector>
namespace trading_engine::runtime {
inline std::uint64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
class LatencyMetrics {
    struct Samples { std::uint64_t total=0; std::vector<std::uint64_t> values; };
    std::map<std::string,Samples> data_;
public:
    void observe(const std::string& name,std::uint64_t value) {
        auto& s=data_[name]; if(s.values.size()<100000) s.values.push_back(value); else s.values[s.total%100000]=value; ++s.total;
    }
    boost::json::object report() const {
        boost::json::object out;
        for(auto& [name,s]:data_) {
            auto sorted=s.values; std::sort(sorted.begin(),sorted.end());
            boost::json::object row{{"observed",s.total},{"samples",sorted.size()},{"unit","ns"},{"window","last_at_most_100000"}};
            for(auto [label,q,min]:{std::tuple{"p50",.5,2},std::tuple{"p95",.95,20},std::tuple{"p99",.99,100},std::tuple{"p99_9",.999,10000}})
                if(sorted.size()>=static_cast<std::size_t>(min)) row[label]=sorted[static_cast<std::size_t>(std::ceil(q*sorted.size()))-1]; else row[label]=nullptr;
            out[name]=row;
        }
        return out;
    }
};
}
