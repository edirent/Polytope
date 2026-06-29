#pragma once

#include "engine/strategy/market_making/fair/VolProvider.h"

#include <atomic>
#include <cstdint>

namespace trading_engine::strategy::market_making {

class FixedVolProvider final : public VolProvider {
public:
    FixedVolProvider(
        double annualized_vol,
        std::int64_t update_ts_ms
    )
        : annualized_vol_(annualized_vol),
          update_ts_ms_(update_ts_ms) {}

    [[nodiscard]] VolSnapshot latest(
        ExternalFairSymbol /*symbol*/
    ) const override {
        VolSnapshot snapshot;
        snapshot.ok = true;
        snapshot.annualized_vol = annualized_vol_.load();
        snapshot.update_ts_ms = update_ts_ms_.load();
        return snapshot;
    }

    void update(
        double annualized_vol,
        std::int64_t update_ts_ms
    ) {
        annualized_vol_.store(annualized_vol);
        update_ts_ms_.store(update_ts_ms);
    }

private:
    std::atomic<double> annualized_vol_{0.0};
    std::atomic<std::int64_t> update_ts_ms_{0};
};

}  // namespace trading_engine::strategy::market_making
