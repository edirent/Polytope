#pragma once

#include "oracle/ingestion/RawMarketRecord.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace trading_engine::oracle {

struct MarketApiFetchOptions {
    std::string endpoint = "https://gamma-api.polymarket.com/markets";
    std::uint32_t limit = 100;
    std::uint32_t offset = 0;

    bool active = true;
    bool closed = false;
    bool archived = false;
    bool require_order_book = true;
};

struct MarketApiFetchResult {
    std::vector<RawMarketRecord> records;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    std::uint32_t response_count = 0;

    [[nodiscard]] bool ok() const noexcept {
        return errors.empty();
    }
};

class MarketApiClient {
public:
    [[nodiscard]] MarketApiFetchResult fetch_active_markets() const;

    [[nodiscard]] MarketApiFetchResult fetch_markets(
        const MarketApiFetchOptions& options
    ) const;
};

[[nodiscard]] MarketApiFetchResult parse_polymarket_gamma_markets(
    std::string_view response_body,
    std::uint64_t fetched_at_ns
);

}  // namespace trading_engine::oracle
