#pragma once

#include "oracle/ingestion/RawMarketRecord.h"

#include <string>
#include <vector>

namespace trading_engine::oracle {

struct MarketDescriptionLoadResult {
    std::vector<RawMarketRecord> records;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    [[nodiscard]] bool ok() const noexcept {
        return errors.empty();
    }
};

class MarketDescriptionLoader {
public:
    [[nodiscard]] MarketDescriptionLoadResult load_jsonl(
        const std::string& path
    ) const;
};

[[nodiscard]] std::string to_jsonl_line(const RawMarketRecord& record);

}  // namespace trading_engine::oracle
