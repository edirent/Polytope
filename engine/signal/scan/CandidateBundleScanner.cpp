#include "engine/signal/scan/CandidateBundleScanner.h"

#include <algorithm>
#include <unordered_set>

namespace trading_engine::signal {

std::vector<const CandidateBundle*> CandidateBundleScanner::full_scan(
    const BundleRegistry& registry
) const {
    std::vector<const CandidateBundle*> out;
    const auto bundles = registry.active_bundles();
    out.reserve(bundles.size());
    for (const auto& bundle : bundles) {
        out.push_back(&bundle);
    }
    return out;
}

std::vector<const CandidateBundle*> CandidateBundleScanner::dirty_asset_scan(
    const BundleRegistry& registry,
    const DirtyAssetSet& dirty_assets
) const {
    std::unordered_set<std::uint64_t> seen;
    std::vector<std::uint64_t> bundle_ids;

    for (const auto& asset_id : dirty_assets.assets()) {
        for (const auto bundle_id : registry.bundles_for_asset(asset_id)) {
            const auto [_, inserted] = seen.insert(bundle_id);
            if (inserted) {
                bundle_ids.push_back(bundle_id);
            }
        }
    }

    std::sort(bundle_ids.begin(), bundle_ids.end());

    std::vector<const CandidateBundle*> out;
    out.reserve(bundle_ids.size());
    for (const auto bundle_id : bundle_ids) {
        if (const auto* bundle = registry.find_bundle(bundle_id)) {
            out.push_back(bundle);
        }
    }
    return out;
}

}  // namespace trading_engine::signal
