#pragma once

#include "oracle/public/ArtifactManifest.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace trading_engine::oracle {

struct OracleArtifactContents {
    ArtifactManifest manifest;

    std::string market_universe_json;
    std::string rulebook_json;

    std::vector<std::byte> variables_bin;
    std::vector<std::byte> constraints_bin;
    std::vector<std::byte> components_bin;
    std::vector<std::byte> component_constraints_bin;
    std::vector<std::byte> oracle_descriptors_bin;
    std::vector<std::byte> bundle_templates_bin;
    std::vector<std::byte> feasible_states_bin;
    std::vector<std::byte> payoff_matrix_bin;
    std::vector<std::byte> candidate_bundles_bin;
    std::vector<std::byte> market_dependency_graph_bin;
    std::vector<std::byte> settlement_bitmask_bin;
};

struct ArtifactExportResult {
    std::filesystem::path artifact_dir;
    std::uint64_t artifact_hash = 0;
    std::map<std::string, std::string> checksums;
    std::vector<std::string> errors;

    [[nodiscard]] bool ok() const noexcept {
        return errors.empty();
    }
};

class ArtifactExporter {
public:
    [[nodiscard]] ArtifactExportResult export_artifact(
        const std::filesystem::path& root,
        const std::string& artifact_id,
        const OracleArtifactContents& contents
    ) const;
};

}  // namespace trading_engine::oracle
