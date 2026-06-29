#pragma once

#include "engine/risk/public/QuoteRiskDecision.h"
#include "engine/strategy/market_making/canonical/CanonicalMarketState.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace trading_engine::strategy::market_making::research {

struct CanonicalQuoteResearchRow {
    std::int64_t ts_ms = 0;
    CanonicalMarketState state;

    std::int64_t canonical_yes_market_mid_tick = 0;
    std::int64_t canonical_yes_external_raw_tick = 0;
    std::int64_t canonical_yes_tradable_fair_tick = 0;
    std::int64_t asset_external_raw_tick = 0;
    std::int64_t asset_tradable_fair_tick = 0;

    std::int64_t basis_raw_tick = 0;
    std::int64_t basis_tradable_tick = 0;
    std::int64_t buy_edge_tick = 0;
    std::int64_t sell_edge_tick = 0;

    std::int64_t current_inventory_asset = 0;
    std::int64_t current_inventory_canonical_yes = 0;
    std::int64_t target_inventory_canonical_yes = 0;
    std::int64_t portfolio_touch_exposure = 0;

    std::string quote_side;
    std::int64_t quote_price_tick = 0;
    std::int64_t quote_size_lots = 0;
    std::string quote_reason;
    std::string risk_decision;
    std::string risk_reject_reason;
    std::int64_t latency_ns = 0;
};

class CanonicalQuoteResearchLogger {
public:
    explicit CanonicalQuoteResearchLogger(std::filesystem::path path);

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

    void log(const CanonicalQuoteResearchRow& row);

private:
    static const char* outcome_side_name(OutcomeSide side) noexcept;
    static const char* event_type_name(ExternalFairEventType type) noexcept;
    static std::string csv_escape(const std::string& value);

    std::filesystem::path path_;
    std::ofstream out_;
    mutable std::mutex mutex_;
};

}  // namespace trading_engine::strategy::market_making::research
