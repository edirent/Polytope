#pragma once

#include "engine/signal/reader/MarketSnapshotReader.h"
#include "state/MarketStateView.h"

namespace trading_engine::signal {

class MarketStateViewSnapshotReader final : public IMarketSnapshotReader {
public:
    explicit MarketStateViewSnapshotReader(
        const trading_engine::state::MarketStateView& view
    ) noexcept;

    [[nodiscard]] SnapshotReadResult read_for_bundle(
        const CandidateBundle& bundle,
        const SignalConfig& config
    ) const override;

private:
    const trading_engine::state::MarketStateView& view_;
};

}  // namespace trading_engine::signal
