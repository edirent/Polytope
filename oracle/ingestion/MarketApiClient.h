#pragma once

#include "oracle/ingestion/RawMarketRecord.h"

#include <string>
#include <vector>

namespace trading_engine::oracle {

struct MarketApiFetchResult {
    std::vector<RawMarketRecord> records;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    [[nodiscard]] bool ok() const noexcept {
        return errors.empty();
    }
};

class MarketApiClient {
public:
    [[nodiscard]] MarketApiFetchResult fetch_active_markets() const;
};

}  // namespace trading_engine::oracle
