#pragma once

#include "state/EntityStateStore.h"
#include "state/chain/ConfirmedTradeState.h"
#include "state/quality/BookQualityState.h"
#include "state/quality/ReconciliationState.h"

#include <cstdint>

namespace trading_engine::state {

struct DataQualityGateConfig {
    std::uint64_t ws_stale_timeout_ns{2'000'000'000ULL};
    std::uint64_t chain_lag_timeout_ns{10'000'000'000ULL};
};

struct DataQualityInput {
    const EntityState* entity{nullptr};
    const ConfirmedTradeState* confirmed_trade_state{nullptr};
    ReconciliationState reconciliation;

    std::uint64_t now_ns{0};

    std::uint32_t ws_decode_errors_recent{0};
    std::uint32_t state_errors_recent{0};
    std::uint32_t chain_decode_errors_recent{0};
};

class DataQualityGate {
public:
    explicit DataQualityGate(
        DataQualityGateConfig config = DataQualityGateConfig{}
    ) noexcept;

    [[nodiscard]] BookQualityState evaluate(
        const DataQualityInput& input
    ) const noexcept;

private:
    [[nodiscard]] bool ws_live(const DataQualityInput& input) const noexcept;
    [[nodiscard]] bool chain_live(
        const DataQualityInput& input
    ) const noexcept;

private:
    DataQualityGateConfig config_;
};

}  // namespace trading_engine::state
