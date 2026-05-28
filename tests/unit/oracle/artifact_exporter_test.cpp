#include "oracle/artifact/ArtifactExporter.h"
#include "oracle/artifact/ArtifactLayout.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::oracle::ArtifactExporter;
using trading_engine::oracle::OracleArtifactContents;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

template <typename Actual, typename Expected>
void expect_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& field
) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
}

std::vector<std::byte> bytes(std::initializer_list<unsigned char> values) {
    std::vector<std::byte> out;
    for (const auto value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

OracleArtifactContents fixture_contents() {
    OracleArtifactContents contents;
    contents.manifest.created_at_ns = 7;
    contents.manifest.market_count = 1;
    contents.manifest.asset_count = 2;
    contents.manifest.variable_count = 2;
    contents.manifest.rule_count = 1;
    contents.manifest.constraint_count = 1;
    contents.manifest.feasible_state_count = 2;
    contents.manifest.bundle_count = 1;
    contents.manifest.llm_enabled = false;
    contents.manifest.llm_outputs_used = false;
    contents.manifest.llm_outputs_require_manual_review = false;
    contents.manifest.llm_provider = "none";
    contents.manifest.input_snapshot_hash = "input_hash";
    contents.manifest.rulebook_hash = "rulebook_hash";
    contents.manifest.constraint_hash = "constraint_hash";
    contents.manifest.feasible_states_hash = "states_hash";
    contents.manifest.payoff_hash = "payoff_hash";
    contents.manifest.bundle_hash = "bundle_hash";

    contents.market_universe_json = "{\"markets\":[\"m1\"]}\n";
    contents.rulebook_json = "{\"rules\":[\"r1\"]}\n";
    contents.variables_bin = bytes({1, 2, 3});
    contents.constraints_bin = bytes({4, 5});
    contents.feasible_states_bin = bytes({6});
    contents.payoff_matrix_bin = bytes({7, 8, 9});
    contents.candidate_bundles_bin = bytes({10, 11});
    contents.market_dependency_graph_bin = bytes({12});
    contents.settlement_bitmask_bin = bytes({13});
    return contents;
}

std::filesystem::path test_root(const std::string& name) {
    const auto root =
        std::filesystem::temp_directory_path() / ("oracle_" + name);
    std::filesystem::remove_all(root);
    return root;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail("failed to open file: " + path.string());
    }
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
}

void ArtifactExporter_WritesAllFiles() {
    ArtifactExporter exporter;
    const auto result = exporter.export_artifact(
        test_root("artifact_exporter_all_files"),
        "artifact_a",
        fixture_contents()
    );

    expect_true(result.ok(), "export ok");
    for (const auto filename : trading_engine::oracle::kArtifactPayloadFiles) {
        expect_true(
            std::filesystem::exists(result.artifact_dir / std::string{filename}),
            std::string{filename}
        );
    }
    expect_true(
        std::filesystem::exists(
            result.artifact_dir /
            std::string{trading_engine::oracle::kChecksumsFile}
        ),
        "checksums.txt"
    );
}

void ArtifactExporter_WritesManifest() {
    ArtifactExporter exporter;
    const auto result = exporter.export_artifact(
        test_root("artifact_exporter_manifest"),
        "artifact_a",
        fixture_contents()
    );

    expect_true(result.ok(), "export ok");
    const auto manifest =
        read_file(result.artifact_dir / std::string{trading_engine::oracle::kManifestFile});
    expect_true(
        manifest.find("\"llm_enabled\":false") != std::string::npos,
        "llm_enabled"
    );
    expect_true(
        manifest.find("\"llm_outputs_require_manual_review\":false") !=
            std::string::npos,
        "llm manual review"
    );
    expect_true(
        manifest.find("\"input_snapshot_hash\":\"input_hash\"") !=
            std::string::npos,
        "input hash"
    );
}

void ArtifactExporter_WritesChecksums() {
    ArtifactExporter exporter;
    const auto result = exporter.export_artifact(
        test_root("artifact_exporter_checksums"),
        "artifact_a",
        fixture_contents()
    );

    expect_true(result.ok(), "export ok");
    expect_equal(
        result.checksums.size(),
        trading_engine::oracle::kChecksumFiles.size(),
        "checksum count"
    );

    const auto checksums =
        read_file(result.artifact_dir / std::string{trading_engine::oracle::kChecksumsFile});
    for (const auto filename : trading_engine::oracle::kChecksumFiles) {
        expect_true(
            checksums.find(std::string{filename} + " ") != std::string::npos,
            std::string{filename}
        );
    }
}

void ArtifactExporter_DeterministicOutput() {
    ArtifactExporter exporter;
    const auto first = exporter.export_artifact(
        test_root("artifact_exporter_deterministic_a"),
        "artifact_a",
        fixture_contents()
    );
    const auto second = exporter.export_artifact(
        test_root("artifact_exporter_deterministic_b"),
        "artifact_b",
        fixture_contents()
    );

    expect_true(first.ok(), "first export ok");
    expect_true(second.ok(), "second export ok");
    expect_equal(first.checksums, second.checksums, "checksums");
    expect_equal(first.artifact_hash, second.artifact_hash, "artifact hash");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"ArtifactExporter_WritesAllFiles", &ArtifactExporter_WritesAllFiles},
        {"ArtifactExporter_WritesManifest", &ArtifactExporter_WritesManifest},
        {"ArtifactExporter_WritesChecksums", &ArtifactExporter_WritesChecksums},
        {
            "ArtifactExporter_DeterministicOutput",
            &ArtifactExporter_DeterministicOutput
        }
    };
    return test_map;
}

int run_test(const std::string& name) {
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << name << " failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << name << " passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        return run_test(argv[1]);
    }

    int failures = 0;
    for (const auto& [name, _] : tests()) {
        failures += run_test(name) == 0 ? 0 : 1;
    }
    return failures == 0 ? 0 : 1;
}
