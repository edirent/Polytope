#pragma once

#include "engine/signal/reader/MarketSnapshotReader.h"

#include <filesystem>
#include <unordered_map>

namespace trading_engine::signal {

class FixtureMarketSnapshotReader final : public IMarketSnapshotReader {
public:
    [[nodiscard]] bool load(
        const std::filesystem::path& path,
        std::string* error = nullptr
    );

    [[nodiscard]] SnapshotReadResult read_for_bundle(
        const CandidateBundle& bundle,
        const SignalConfig& config
    ) const override;

private:
    std::unordered_map<std::string, MarketStateSnapshot> snapshots_;
};

}  // namespace trading_engine::signal
