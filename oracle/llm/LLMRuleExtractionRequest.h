#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::oracle {

struct LLMMarketContext {
    std::string market_id;
    std::string event_id;
    std::string title;
    std::string description;
    std::vector<std::string> outcomes;
    std::vector<std::string> asset_ids;
    std::string resolution_source;
};

struct LLMRuleExtractionRequest {
    std::vector<LLMMarketContext> markets;
    std::string instruction;
    std::uint64_t requested_at_ns = 0;
};

}  // namespace trading_engine::oracle
