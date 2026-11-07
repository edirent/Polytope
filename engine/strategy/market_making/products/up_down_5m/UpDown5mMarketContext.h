#pragma once

#include "engine/strategy/market_making/public/MarketMakingTypes.h"

#include <cstdint>
#include <string>

namespace trading_engine::strategy::market_making::products::up_down_5m {

struct UpDown5mMarketContext {
    std::string market_id;
    std::string up_token_id;
    std::string down_token_id;

    ProductDirection asset_direction = ProductDirection::Unknown;

    std::int64_t round_start_ms = 0;
    std::int64_t round_expiry_ms = 0;
    std::int64_t now_ms = 0;

    double start_spot = 0.0;
    double current_spot = 0.0;
    double sigma_annualized = 0.0;
    double oracle_age_ms = 0.0;

    double polymarket_up_mid = 0.0;
    double polymarket_down_mid = 0.0;

    bool settlement_source_ok = false;
    bool book_ok = false;
};

}  // namespace trading_engine::strategy::market_making::products::up_down_5m
