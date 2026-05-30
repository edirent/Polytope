#include "oracle/artifact/ArtifactLoader.h"

#include "oracle/artifact/ArtifactChecksum.h"
#include "oracle/artifact/ArtifactLayout.h"

#include <boost/json.hpp>

#include <array>
#include <fstream>
#include <sstream>

namespace trading_engine::oracle {

namespace {

namespace json = boost::json;

inline constexpr std::array<std::string_view, 8> kLegacyRequiredChecksumFiles{
    kManifestFile,
    kMarketUniverseFile,
    kRulebookFile,
    kVariablesFile,
    kConstraintsFile,
    kFeasibleStatesFile,
    kPayoffMatrixFile,
    kCandidateBundlesFile
};

std::string read_text(
    const std::filesystem::path& path,
    std::vector<std::string>* errors
) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        errors->push_back("failed to open artifact file: " + path.string());
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        errors->push_back("failed to read artifact file: " + path.string());
        return {};
    }
    return buffer.str();
}

std::vector<std::byte> read_bytes(
    const std::filesystem::path& path,
    std::vector<std::string>* errors
) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        errors->push_back("failed to open artifact file: " + path.string());
        return {};
    }

    std::vector<std::byte> bytes;
    char value = 0;
    while (input.get(value)) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    if (!input.eof()) {
        errors->push_back("failed to read artifact file: " + path.string());
    }
    return bytes;
}

std::vector<std::byte> read_optional_bytes(
    const std::filesystem::path& path,
    std::vector<std::string>* errors
) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return {};
    }
    return read_bytes(path, errors);
}

std::uint32_t u32_field(const json::object& object, const char* name) {
    const auto it = object.find(name);
    if (it == object.end()) {
        return 0;
    }
    if (it->value().is_uint64()) {
        return static_cast<std::uint32_t>(it->value().as_uint64());
    }
    if (it->value().is_int64() && it->value().as_int64() >= 0) {
        return static_cast<std::uint32_t>(it->value().as_int64());
    }
    return 0;
}

std::uint64_t u64_field(const json::object& object, const char* name) {
    const auto it = object.find(name);
    if (it == object.end()) {
        return 0;
    }
    if (it->value().is_uint64()) {
        return it->value().as_uint64();
    }
    if (it->value().is_int64() && it->value().as_int64() >= 0) {
        return static_cast<std::uint64_t>(it->value().as_int64());
    }
    return 0;
}

bool bool_field(const json::object& object, const char* name) {
    const auto it = object.find(name);
    return it != object.end() && it->value().is_bool() &&
           it->value().as_bool();
}

std::string string_field(const json::object& object, const char* name) {
    const auto it = object.find(name);
    if (it == object.end() || !it->value().is_string()) {
        return {};
    }
    return json::value_to<std::string>(it->value());
}

ArtifactManifest parse_manifest(
    const std::string& content,
    std::vector<std::string>* errors
) {
    ArtifactManifest manifest;

    boost::json::error_code error;
    const auto parsed = json::parse(content, error);
    if (error || !parsed.is_object()) {
        errors->push_back("malformed manifest.json");
        return manifest;
    }

    const auto& object = parsed.as_object();
    manifest.artifact_version = u32_field(object, "artifact_version");
    manifest.created_at_ns = u64_field(object, "created_at_ns");
    manifest.market_count = u32_field(object, "market_count");
    manifest.asset_count = u32_field(object, "asset_count");
    manifest.variable_count = u32_field(object, "variable_count");
    manifest.rule_count = u32_field(object, "rule_count");
    manifest.constraint_count = u32_field(object, "constraint_count");
    manifest.component_count = u32_field(object, "component_count");
    manifest.enumerable_component_count =
        u32_field(object, "enumerable_component_count");
    manifest.semantic_oracle_component_count =
        u32_field(object, "semantic_oracle_component_count");
    manifest.fallback_oracle_component_count =
        u32_field(object, "fallback_oracle_component_count");
    manifest.feasible_state_count = u64_field(object, "feasible_state_count");
    manifest.bundle_count = u64_field(object, "bundle_count");
    manifest.llm_enabled = bool_field(object, "llm_enabled");
    manifest.llm_outputs_used = bool_field(object, "llm_outputs_used");
    manifest.llm_outputs_require_manual_review =
        bool_field(object, "llm_outputs_require_manual_review");
    manifest.llm_provider = string_field(object, "llm_provider");
    manifest.input_snapshot_hash = string_field(object, "input_snapshot_hash");
    manifest.rulebook_hash = string_field(object, "rulebook_hash");
    manifest.constraint_hash = string_field(object, "constraint_hash");
    manifest.constraint_graph_hash =
        string_field(object, "constraint_graph_hash");
    manifest.component_partition_hash =
        string_field(object, "component_partition_hash");
    manifest.oracle_descriptor_hash =
        string_field(object, "oracle_descriptor_hash");
    manifest.bundle_template_hash =
        string_field(object, "bundle_template_hash");
    manifest.feasible_states_hash = string_field(object, "feasible_states_hash");
    manifest.payoff_hash = string_field(object, "payoff_hash");
    manifest.bundle_hash = string_field(object, "bundle_hash");
    return manifest;
}

std::map<std::string, std::string> parse_checksums(
    const std::string& content,
    std::vector<std::string>* errors
) {
    std::map<std::string, std::string> checksums;
    std::istringstream input(content);
    std::string filename;
    std::string hash;
    while (input >> filename >> hash) {
        checksums[filename] = hash;
    }

    for (const auto expected : kLegacyRequiredChecksumFiles) {
        if (!checksums.contains(std::string{expected})) {
            errors->push_back(
                "missing checksum entry: " + std::string{expected}
            );
        }
    }
    return checksums;
}

bool verify_checksums(
    const std::filesystem::path& artifact_dir,
    const std::map<std::string, std::string>& checksums,
    std::vector<std::string>* errors
) {
    bool ok = true;
    for (const auto& [name, expected_hash] : checksums) {
        const auto checksum = checksum_file(artifact_dir / name);
        if (!checksum.ok()) {
            errors->push_back(checksum.error);
            ok = false;
            continue;
        }
        if (checksum.hex != expected_hash) {
            errors->push_back("checksum mismatch: " + name);
            ok = false;
        }
    }
    return ok;
}

}  // namespace

ArtifactLoadResult ArtifactLoader::load(
    const std::filesystem::path& artifact_dir
) const {
    ArtifactLoadResult result;

    const auto checksum_text = read_text(
        artifact_dir / std::string{kChecksumsFile},
        &result.errors
    );
    result.checksums = parse_checksums(checksum_text, &result.errors);
    result.checksums_ok =
        verify_checksums(artifact_dir, result.checksums, &result.errors);

    const auto manifest_text = read_text(
        artifact_dir / std::string{kManifestFile},
        &result.errors
    );
    result.contents.manifest = parse_manifest(manifest_text, &result.errors);
    result.contents.market_universe_json =
        read_text(artifact_dir / std::string{kMarketUniverseFile}, &result.errors);
    result.contents.rulebook_json =
        read_text(artifact_dir / std::string{kRulebookFile}, &result.errors);
    result.contents.variables_bin =
        read_bytes(artifact_dir / std::string{kVariablesFile}, &result.errors);
    result.contents.constraints_bin =
        read_bytes(artifact_dir / std::string{kConstraintsFile}, &result.errors);
    result.contents.components_bin =
        read_optional_bytes(
            artifact_dir / std::string{kComponentsFile},
            &result.errors
        );
    result.contents.component_constraints_bin =
        read_optional_bytes(
            artifact_dir / std::string{kComponentConstraintsFile},
            &result.errors
        );
    result.contents.oracle_descriptors_bin =
        read_optional_bytes(
            artifact_dir / std::string{kOracleDescriptorsFile},
            &result.errors
        );
    result.contents.bundle_templates_bin =
        read_optional_bytes(
            artifact_dir / std::string{kBundleTemplatesFile},
            &result.errors
        );
    result.contents.feasible_states_bin =
        read_bytes(artifact_dir / std::string{kFeasibleStatesFile}, &result.errors);
    result.contents.payoff_matrix_bin =
        read_bytes(artifact_dir / std::string{kPayoffMatrixFile}, &result.errors);
    result.contents.candidate_bundles_bin =
        read_bytes(artifact_dir / std::string{kCandidateBundlesFile}, &result.errors);
    result.contents.market_dependency_graph_bin =
        read_bytes(
            artifact_dir / std::string{kMarketDependencyGraphFile},
            &result.errors
        );
    result.contents.settlement_bitmask_bin =
        read_bytes(
            artifact_dir / std::string{kSettlementBitmaskFile},
            &result.errors
        );

    return result;
}

}  // namespace trading_engine::oracle
