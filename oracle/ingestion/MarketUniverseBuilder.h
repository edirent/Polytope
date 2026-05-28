#pragma once

#include "oracle/ingestion/RawMarketRecord.h"
#include "oracle/public/ArtifactManifest.h"

#include <string>
#include <vector>

namespace trading_engine::oracle {

struct MarketUniverse {
    std::vector<RawMarketRecord> markets;
    ArtifactManifest manifest;
};

struct MarketUniverseBuildResult {
    MarketUniverse universe;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    [[nodiscard]] bool ok() const noexcept {
        return errors.empty();
    }
};

class MarketUniverseBuilder {
public:
    [[nodiscard]] MarketUniverseBuildResult build(
        const std::vector<RawMarketRecord>& records
    ) const;
};

}  // namespace trading_engine::oracle
