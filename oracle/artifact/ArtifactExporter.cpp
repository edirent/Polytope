#include "oracle/artifact/ArtifactExporter.h"

#include "oracle/artifact/ArtifactChecksum.h"
#include "oracle/artifact/ArtifactLayout.h"
#include "oracle/artifact/ArtifactWriter.h"

#include <boost/json.hpp>

#include <filesystem>
#include <sstream>

namespace trading_engine::oracle {

namespace {

namespace json = boost::json;

json::object manifest_to_json(const ArtifactManifest& manifest) {
    json::object object;
    object["artifact_version"] = manifest.artifact_version;
    object["created_at_ns"] = manifest.created_at_ns;
    object["market_count"] = manifest.market_count;
    object["asset_count"] = manifest.asset_count;
    object["variable_count"] = manifest.variable_count;
    object["rule_count"] = manifest.rule_count;
    object["constraint_count"] = manifest.constraint_count;
    object["component_count"] = manifest.component_count;
    object["enumerable_component_count"] =
        manifest.enumerable_component_count;
    object["semantic_oracle_component_count"] =
        manifest.semantic_oracle_component_count;
    object["fallback_oracle_component_count"] =
        manifest.fallback_oracle_component_count;
    object["feasible_state_count"] = manifest.feasible_state_count;
    object["bundle_count"] = manifest.bundle_count;
    object["llm_enabled"] = manifest.llm_enabled;
    object["llm_outputs_used"] = manifest.llm_outputs_used;
    object["llm_outputs_require_manual_review"] =
        manifest.llm_outputs_require_manual_review;
    object["llm_provider"] = manifest.llm_provider;
    object["input_snapshot_hash"] = manifest.input_snapshot_hash;
    object["rulebook_hash"] = manifest.rulebook_hash;
    object["constraint_hash"] = manifest.constraint_hash;
    object["constraint_graph_hash"] = manifest.constraint_graph_hash;
    object["component_partition_hash"] =
        manifest.component_partition_hash;
    object["oracle_descriptor_hash"] = manifest.oracle_descriptor_hash;
    object["bundle_template_hash"] = manifest.bundle_template_hash;
    object["feasible_states_hash"] = manifest.feasible_states_hash;
    object["payoff_hash"] = manifest.payoff_hash;
    object["bundle_hash"] = manifest.bundle_hash;
    return object;
}

std::string serialize_manifest(const ArtifactManifest& manifest) {
    return json::serialize(manifest_to_json(manifest)) + "\n";
}

bool write_payloads(
    const ArtifactLayout& layout,
    const OracleArtifactContents& contents,
    ArtifactExportResult* result
) {
    if (!ArtifactWriter::write_text(
            layout.file(kManifestFile),
            serialize_manifest(contents.manifest),
            &result->errors
        )) {
        return false;
    }
    if (!ArtifactWriter::write_text(
            layout.file(kMarketUniverseFile),
            contents.market_universe_json,
            &result->errors
        )) {
        return false;
    }
    if (!ArtifactWriter::write_text(
            layout.file(kRulebookFile),
            contents.rulebook_json,
            &result->errors
        )) {
        return false;
    }
    if (!ArtifactWriter::write_bytes(
            layout.file(kVariablesFile),
            contents.variables_bin,
            &result->errors
        )) {
        return false;
    }
    if (!ArtifactWriter::write_bytes(
            layout.file(kConstraintsFile),
            contents.constraints_bin,
            &result->errors
        )) {
        return false;
    }
    if (!ArtifactWriter::write_bytes(
            layout.file(kComponentsFile),
            contents.components_bin,
            &result->errors
        )) {
        return false;
    }
    if (!ArtifactWriter::write_bytes(
            layout.file(kComponentConstraintsFile),
            contents.component_constraints_bin,
            &result->errors
        )) {
        return false;
    }
    if (!ArtifactWriter::write_bytes(
            layout.file(kOracleDescriptorsFile),
            contents.oracle_descriptors_bin,
            &result->errors
        )) {
        return false;
    }
    if (!ArtifactWriter::write_bytes(
            layout.file(kBundleTemplatesFile),
            contents.bundle_templates_bin,
            &result->errors
        )) {
        return false;
    }
    if (!ArtifactWriter::write_bytes(
            layout.file(kFeasibleStatesFile),
            contents.feasible_states_bin,
            &result->errors
        )) {
        return false;
    }
    if (!ArtifactWriter::write_bytes(
            layout.file(kPayoffMatrixFile),
            contents.payoff_matrix_bin,
            &result->errors
        )) {
        return false;
    }
    if (!ArtifactWriter::write_bytes(
            layout.file(kCandidateBundlesFile),
            contents.candidate_bundles_bin,
            &result->errors
        )) {
        return false;
    }
    if (!ArtifactWriter::write_bytes(
            layout.file(kMarketDependencyGraphFile),
            contents.market_dependency_graph_bin,
            &result->errors
        )) {
        return false;
    }
    return ArtifactWriter::write_bytes(
        layout.file(kSettlementBitmaskFile),
        contents.settlement_bitmask_bin,
        &result->errors
    );
}

bool write_checksums(
    const ArtifactLayout& layout,
    ArtifactExportResult* result
) {
    std::ostringstream checksums_text;
    for (const auto filename : kChecksumFiles) {
        const auto checksum = checksum_file(layout.file(filename));
        if (!checksum.ok()) {
            result->errors.push_back(checksum.error);
            return false;
        }

        const std::string name{filename};
        result->checksums[name] = checksum.hex;
        checksums_text << name << ' ' << checksum.hex << '\n';
    }

    const auto text = checksums_text.str();
    result->artifact_hash = fnv1a64(text);
    return ArtifactWriter::write_text(
        layout.file(kChecksumsFile),
        text,
        &result->errors
    );
}

}  // namespace

ArtifactExportResult ArtifactExporter::export_artifact(
    const std::filesystem::path& root,
    const std::string& artifact_id,
    const OracleArtifactContents& contents
) const {
    ArtifactExportResult result;
    const ArtifactLayout layout{root, artifact_id};
    result.artifact_dir = layout.artifact_dir();

    std::error_code error;
    std::filesystem::remove_all(result.artifact_dir, error);
    error.clear();
    if (!std::filesystem::create_directories(result.artifact_dir, error) &&
        error) {
        result.errors.push_back(
            "failed to create artifact directory: " +
            result.artifact_dir.string()
        );
        return result;
    }

    if (!write_payloads(layout, contents, &result)) {
        return result;
    }
    write_checksums(layout, &result);
    return result;
}

}  // namespace trading_engine::oracle
