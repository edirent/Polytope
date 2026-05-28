#pragma once

#include "oracle/bundles/CandidateBundle.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace trading_engine::oracle {

struct BundleValidationResult {
    std::vector<std::string> errors;
    std::vector<std::uint64_t> duplicate_bundle_ids;

    [[nodiscard]] bool ok() const noexcept {
        return errors.empty() && duplicate_bundle_ids.empty();
    }
};

class BundleValidator {
public:
    [[nodiscard]] BundleValidationResult validate(
        const std::vector<CandidateBundle>& bundles,
        const std::unordered_set<std::string>& known_market_ids,
        const std::unordered_set<std::string>& known_asset_ids
    ) const;
};

}  // namespace trading_engine::oracle
