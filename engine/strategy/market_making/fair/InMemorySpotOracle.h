#pragma once

#include "engine/strategy/market_making/fair/SpotOracle.h"

#include <atomic>
#include <cstdint>

namespace trading_engine::strategy::market_making {

class InMemorySpotOracle final : public SpotOracle {
public:
    [[nodiscard]] SpotSnapshot latest(
        ExternalFairSymbol symbol
    ) const override {
        if (symbol != ExternalFairSymbol::SOL) {
            return {};
        }

        const double bid = sol_bid_.load();
        const double ask = sol_ask_.load();
        const auto exchange_ts_ms = sol_exchange_ts_ms_.load();
        const auto recv_ts_ms = sol_recv_ts_ms_.load();

        if (bid <= 0.0 || ask <= 0.0 || ask < bid) {
            return {};
        }

        SpotSnapshot snapshot;
        snapshot.ok = true;
        snapshot.spot = 0.5 * (bid + ask);
        snapshot.exchange_ts_ms = exchange_ts_ms;
        snapshot.local_recv_ts_ms = recv_ts_ms;
        return snapshot;
    }

    void update_sol_book_ticker(
        double bid,
        double ask,
        std::int64_t exchange_ts_ms,
        std::int64_t local_recv_ts_ms
    ) {
        if (bid <= 0.0 || ask <= 0.0 || ask < bid) {
            return;
        }

        sol_bid_.store(bid);
        sol_ask_.store(ask);
        sol_exchange_ts_ms_.store(exchange_ts_ms);
        sol_recv_ts_ms_.store(local_recv_ts_ms);
    }

private:
    std::atomic<double> sol_bid_{0.0};
    std::atomic<double> sol_ask_{0.0};
    std::atomic<std::int64_t> sol_exchange_ts_ms_{0};
    std::atomic<std::int64_t> sol_recv_ts_ms_{0};
};

}  // namespace trading_engine::strategy::market_making
