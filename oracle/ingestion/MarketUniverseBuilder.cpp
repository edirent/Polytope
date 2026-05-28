#include "oracle/ingestion/MarketUniverseBuilder.h"

#include <set>
#include <unordered_set>

namespace trading_engine::oracle {

MarketUniverseBuildResult MarketUniverseBuilder::build(
    const std::vector<RawMarketRecord>& records
) const {
    MarketUniverseBuildResult result;
    std::unordered_set<std::string> seen_markets;
    std::set<std::string> asset_ids;

    for (const auto& record : records) {
        if (record.market_id.empty()) {
            result.errors.push_back("market record missing market_id");
            continue;
        }

        if (record.outcomes.size() != record.asset_ids.size()) {
            result.errors.push_back(
                "market " + record.market_id +
                " outcomes and asset_ids count mismatch"
            );
            continue;
        }

        const auto [_, inserted] = seen_markets.insert(record.market_id);
        if (!inserted) {
            result.warnings.push_back(
                "duplicate market ignored: " + record.market_id
            );
            continue;
        }

        for (const auto& asset_id : record.asset_ids) {
            if (!asset_id.empty()) {
                asset_ids.insert(asset_id);
            }
        }

        result.universe.markets.push_back(record);
    }

    result.universe.manifest.market_count =
        static_cast<std::uint32_t>(result.universe.markets.size());
    result.universe.manifest.asset_count =
        static_cast<std::uint32_t>(asset_ids.size());
    result.universe.manifest.llm_enabled = false;
    result.universe.manifest.llm_outputs_used = false;
    result.universe.manifest.llm_provider = "none";

    return result;
}

}  // namespace trading_engine::oracle
