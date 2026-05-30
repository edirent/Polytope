#pragma once

#include "oracle/bundles/CandidateBundleGenerator.h"
#include "oracle/ingestion/RawMarketRecord.h"
#include "oracle/rules/Rulebook.h"

#include <vector>

namespace trading_engine::oracle {

class CombinatorialBundleGenerator {
public:
    [[nodiscard]] CandidateBundleLoadResult generate_from_rulebook(
        const std::vector<RawMarketRecord>& markets,
        const Rulebook& rulebook
    ) const;
};

}  // namespace trading_engine::oracle
