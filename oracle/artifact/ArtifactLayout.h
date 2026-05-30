#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace trading_engine::oracle {

inline constexpr std::string_view kManifestFile = "manifest.json";
inline constexpr std::string_view kMarketUniverseFile = "market_universe.json";
inline constexpr std::string_view kRulebookFile = "rulebook.json";
inline constexpr std::string_view kVariablesFile = "variables.bin";
inline constexpr std::string_view kConstraintsFile = "constraints.bin";
inline constexpr std::string_view kComponentsFile = "components.bin";
inline constexpr std::string_view kComponentConstraintsFile =
    "component_constraints.bin";
inline constexpr std::string_view kOracleDescriptorsFile =
    "oracle_descriptors.bin";
inline constexpr std::string_view kBundleTemplatesFile = "bundle_templates.bin";
inline constexpr std::string_view kFeasibleStatesFile = "feasible_states.bin";
inline constexpr std::string_view kPayoffMatrixFile = "payoff_matrix.bin";
inline constexpr std::string_view kCandidateBundlesFile = "candidate_bundles.bin";
inline constexpr std::string_view kMarketDependencyGraphFile =
    "market_dependency_graph.bin";
inline constexpr std::string_view kSettlementBitmaskFile = "settlement_bitmask.bin";
inline constexpr std::string_view kChecksumsFile = "checksums.txt";

inline constexpr std::array<std::string_view, 14> kArtifactPayloadFiles{
    kManifestFile,
    kMarketUniverseFile,
    kRulebookFile,
    kVariablesFile,
    kConstraintsFile,
    kComponentsFile,
    kComponentConstraintsFile,
    kOracleDescriptorsFile,
    kBundleTemplatesFile,
    kFeasibleStatesFile,
    kPayoffMatrixFile,
    kCandidateBundlesFile,
    kMarketDependencyGraphFile,
    kSettlementBitmaskFile
};

inline constexpr std::array<std::string_view, 12> kChecksumFiles{
    kManifestFile,
    kMarketUniverseFile,
    kRulebookFile,
    kVariablesFile,
    kConstraintsFile,
    kComponentsFile,
    kComponentConstraintsFile,
    kOracleDescriptorsFile,
    kBundleTemplatesFile,
    kFeasibleStatesFile,
    kPayoffMatrixFile,
    kCandidateBundlesFile
};

struct ArtifactLayout {
    std::filesystem::path root;
    std::string artifact_id;

    [[nodiscard]] std::filesystem::path artifact_dir() const {
        return root / artifact_id;
    }

    [[nodiscard]] std::filesystem::path file(std::string_view name) const {
        return artifact_dir() / std::string{name};
    }
};

}  // namespace trading_engine::oracle
