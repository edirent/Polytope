#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::oracle {

struct RawMarketRecord {
    std::string market_id;
    std::string event_id;

    std::string title;
    std::string description;

    std::vector<std::string> outcomes;
    std::vector<std::string> asset_ids;

    std::string resolution_source;
    std::string end_time;

    std::vector<std::string> tags;

    std::uint64_t fetched_at_ns = 0;
    std::string source;
};

}  // namespace trading_engine::oracle
