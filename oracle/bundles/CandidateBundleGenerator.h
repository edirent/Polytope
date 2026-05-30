#pragma once

#include "oracle/bundles/CandidateBundle.h"
#include "oracle/ingestion/RawMarketRecord.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace trading_engine::oracle {

struct CandidateBundleLoadResult {
    std::vector<CandidateBundle> bundles;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    std::uint64_t bundle_hash = 0;

    [[nodiscard]] bool ok() const noexcept {
        return errors.empty();
    }
};

class CandidateBundleGenerator {
public:
    [[nodiscard]] CandidateBundleLoadResult load_fixture(
        const std::string& path,
        const std::unordered_set<std::string>& known_market_ids,
        const std::unordered_set<std::string>& known_asset_ids
    ) const;

    [[nodiscard]] CandidateBundleLoadResult generate_buy_all_outcomes(
        const std::vector<RawMarketRecord>& markets,
        const std::unordered_set<std::string>& known_market_ids,
        const std::unordered_set<std::string>& known_asset_ids
    ) const;

    [[nodiscard]] bool export_fixture_artifact(
        const std::vector<CandidateBundle>& bundles,
        const std::string& path,
        std::vector<std::string>* errors = nullptr
    ) const;
};

}  // namespace trading_engine::oracle
