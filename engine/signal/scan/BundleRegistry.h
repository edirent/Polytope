#pragma once

#include "engine/signal/reader/OracleArtifactReader.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading_engine::signal {

class BundleRegistry {
public:
    [[nodiscard]] bool load_from_oracle_reader(
        const OracleArtifactReader& reader
    );

    [[nodiscard]] std::span<const CandidateBundle> active_bundles() const;

    [[nodiscard]] const CandidateBundle* find_bundle(
        std::uint64_t bundle_id
    ) const;

    [[nodiscard]] std::span<const std::uint64_t> bundles_for_asset(
        const std::string& asset_id
    ) const;

    [[nodiscard]] std::uint64_t artifact_hash() const noexcept;
    [[nodiscard]] std::uint64_t bundle_hash(std::uint64_t bundle_id) const;

private:
    std::vector<CandidateBundle> bundles_;
    std::unordered_map<std::uint64_t, std::size_t> bundle_index_;
    std::unordered_map<std::string, std::vector<std::uint64_t>>
        asset_to_bundles_;

    std::uint64_t artifact_hash_ = 0;
};

}  // namespace trading_engine::signal
