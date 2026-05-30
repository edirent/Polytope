#include "engine/signal/scan/BundleRegistry.h"

#include "oracle/bundles/BundleHash.h"

#include <algorithm>

namespace trading_engine::signal {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hash_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
        *hash *= kFnvPrime;
    }
}

[[nodiscard]] std::uint64_t registry_artifact_hash(
    const OracleArtifactReader& reader
) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_u64(&hash, reader.artifact_version());
    hash_u64(&hash, reader.bundle_hash());
    return hash;
}

}  // namespace

bool BundleRegistry::load_from_oracle_reader(
    const OracleArtifactReader& reader
) {
    if (reader.artifact_version() == 0) {
        bundles_.clear();
        bundle_index_.clear();
        asset_to_bundles_.clear();
        artifact_hash_ = 0;
        return false;
    }

    bundles_.assign(
        reader.active_bundles().begin(),
        reader.active_bundles().end()
    );
    std::sort(
        bundles_.begin(),
        bundles_.end(),
        [](const CandidateBundle& lhs, const CandidateBundle& rhs) {
            return lhs.bundle_id < rhs.bundle_id;
        }
    );

    bundle_index_.clear();
    asset_to_bundles_.clear();
    artifact_hash_ = 0;

    for (std::size_t i = 0; i < bundles_.size(); ++i) {
        const auto& bundle = bundles_[i];
        const auto [_, inserted] = bundle_index_.emplace(bundle.bundle_id, i);
        if (!inserted) {
            bundles_.clear();
            bundle_index_.clear();
            asset_to_bundles_.clear();
            return false;
        }

        for (std::uint16_t leg_index = 0;
             leg_index < bundle.leg_count &&
             leg_index < trading_engine::oracle::kMaxBundleLegs;
             ++leg_index) {
            const auto& asset_id = bundle.legs[leg_index].asset_id;
            if (!asset_id.empty()) {
                asset_to_bundles_[asset_id].push_back(bundle.bundle_id);
            }
        }
    }

    for (auto& [_, bundle_ids] : asset_to_bundles_) {
        std::sort(bundle_ids.begin(), bundle_ids.end());
        bundle_ids.erase(
            std::unique(bundle_ids.begin(), bundle_ids.end()),
            bundle_ids.end()
        );
    }

    artifact_hash_ = registry_artifact_hash(reader);
    return true;
}

std::span<const CandidateBundle> BundleRegistry::active_bundles() const {
    return bundles_;
}

const CandidateBundle* BundleRegistry::find_bundle(
    std::uint64_t bundle_id
) const {
    const auto it = bundle_index_.find(bundle_id);
    if (it == bundle_index_.end()) {
        return nullptr;
    }
    return &bundles_[it->second];
}

std::span<const std::uint64_t> BundleRegistry::bundles_for_asset(
    const std::string& asset_id
) const {
    const auto it = asset_to_bundles_.find(asset_id);
    if (it == asset_to_bundles_.end()) {
        return {};
    }
    return it->second;
}

std::uint64_t BundleRegistry::artifact_hash() const noexcept {
    return artifact_hash_;
}

std::uint64_t BundleRegistry::bundle_hash(std::uint64_t bundle_id) const {
    const auto* bundle = find_bundle(bundle_id);
    if (!bundle) {
        return 0;
    }
    return trading_engine::oracle::hash_candidate_bundle(*bundle);
}

}  // namespace trading_engine::signal
