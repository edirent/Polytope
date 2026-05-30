#pragma once

#include "engine/signal/scan/BundleRegistry.h"
#include "engine/signal/scan/DirtyAssetSet.h"

#include <vector>

namespace trading_engine::signal {

enum class CandidateScanMode : std::uint8_t {
    FullScan,
    DirtyAssetScan
};

class CandidateBundleScanner {
public:
    [[nodiscard]] std::vector<const CandidateBundle*> full_scan(
        const BundleRegistry& registry
    ) const;

    [[nodiscard]] std::vector<const CandidateBundle*> dirty_asset_scan(
        const BundleRegistry& registry,
        const DirtyAssetSet& dirty_assets
    ) const;
};

}  // namespace trading_engine::signal
