#pragma once

#include "engine/strategy/market_making/fair/ExternalFairMarketSpec.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace trading_engine::strategy::market_making::research {

struct ExternalFairBasisSnapshot {
    std::int64_t ts_ms = 0;
    std::string market_id;
    std::string token_id;
    ExternalFairSymbol symbol = ExternalFairSymbol::Unknown;
    ExternalFairEventType event_type = ExternalFairEventType::Unknown;
    double barrier_price = 0.0;
    OutcomeSide outcome_side = OutcomeSide::Yes;
    double spot = 0.0;
    double annualized_vol = 0.0;
    double tte_years = 0.0;
    std::int64_t external_fair_tick = 0;
    double yes_probability = 0.0;
    std::int64_t best_bid_tick = 0;
    std::int64_t best_ask_tick = 0;
    std::int64_t bid_size = 0;
    std::int64_t ask_size = 0;
    std::int64_t book_mid_tick = 0;
    std::int64_t book_micro_tick = 0;
    std::int64_t spread_tick = 0;
    std::int64_t mid_basis_tick = 0;
    std::int64_t micro_basis_tick = 0;
    std::int64_t buy_edge_tick = 0;
    std::int64_t sell_edge_tick = 0;
    std::int64_t book_age_ms = 0;
    std::int64_t spot_age_ms = 0;
    std::int64_t vol_age_ms = 0;
    std::string spot_source;
};

class ExternalFairBasisLogger {
public:
    explicit ExternalFairBasisLogger(std::filesystem::path path);

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

    void log(const ExternalFairBasisSnapshot& snapshot);

private:
    static const char* symbol_name(ExternalFairSymbol symbol) noexcept;
    static const char* event_type_name(ExternalFairEventType event_type) noexcept;
    static const char* outcome_side_name(OutcomeSide side) noexcept;
    static std::string csv_escape(const std::string& value);

    std::filesystem::path path_;
    std::ofstream out_;
    mutable std::mutex mutex_;
};

}  // namespace trading_engine::strategy::market_making::research
