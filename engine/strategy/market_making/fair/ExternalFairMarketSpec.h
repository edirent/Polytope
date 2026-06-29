#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace trading_engine::strategy::market_making {

enum class ExternalFairSymbol {
    Unknown = 0,
    BTC,
    ETH,
    SOL,
};

enum class ExternalFairEventType {
    Unknown = 0,
    TerminalAbove,
    TerminalBelow,
    UpTouch,
    DownTouch,
};

enum class OutcomeSide {
    Yes = 0,
    No = 1,
};

struct ExternalFairMarketSpec {
    std::string market_id;
    std::string token_id;

    ExternalFairSymbol symbol = ExternalFairSymbol::Unknown;
    ExternalFairEventType event_type = ExternalFairEventType::Unknown;
    OutcomeSide outcome_side = OutcomeSide::Yes;

    double barrier_price = 0.0;
    std::int64_t expiry_unix_ms = 0;

    std::int64_t max_spot_staleness_ms = 1'500;
    std::int64_t max_vol_staleness_ms = 60'000;

    double min_annualized_vol = 0.20;
    double max_annualized_vol = 4.00;

    std::int64_t price_scale_tick = 10'000;
};

struct ExternalFairResult {
    bool ok = false;
    std::int64_t fair_value_tick = 0;
    double yes_probability = 0.0;
    std::string reject_reason;

    static ExternalFairResult reject(std::string reason) {
        ExternalFairResult result;
        result.reject_reason = std::move(reason);
        return result;
    }

    static ExternalFairResult success(
        std::int64_t fair_tick,
        double yes_prob
    ) {
        ExternalFairResult result;
        result.ok = true;
        result.fair_value_tick = fair_tick;
        result.yes_probability = yes_prob;
        return result;
    }
};

}  // namespace trading_engine::strategy::market_making
