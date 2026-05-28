#include "oracle/artifact/ArtifactExporter.h"
#include "oracle/artifact/ArtifactLoader.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::oracle::ArtifactExporter;
using trading_engine::oracle::ArtifactLoader;
using trading_engine::oracle::OracleArtifactContents;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
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
    contents.manifest.created_at_ns = 11;
    contents.manifest.market_count = 1;
    contents.manifest.asset_count = 2;
    contents.manifest.variable_count = 2;
    contents.manifest.rule_count = 1;
    contents.manifest.constraint_count = 1;
    contents.manifest.feasible_state_count = 2;
    contents.manifest.bundle_count = 1;
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
    contents.payoff_matrix_bin = bytes({7, 8});
    contents.candidate_bundles_bin = bytes({9});
    contents.market_dependency_graph_bin = bytes({10});
    contents.settlement_bitmask_bin = bytes({11});
    return contents;
}

std::filesystem::path test_root(const std::string& name) {
    const auto root =
        std::filesystem::temp_directory_path() / ("oracle_" + name);
    std::filesystem::remove_all(root);
    return root;
}

std::filesystem::path export_fixture(const std::string& name) {
    ArtifactExporter exporter;
    const auto result = exporter.export_artifact(
        test_root(name),
        "artifact_a",
        fixture_contents()
    );
    expect_true(result.ok(), "export ok");
    return result.artifact_dir;
}

void ArtifactLoader_RoundTripsArtifact() {
    ArtifactLoader loader;
    const auto loaded = loader.load(
        export_fixture("artifact_loader_roundtrip")
    );

    expect_true(loaded.ok(), "load ok");
    expect_true(loaded.checksums_ok, "checksums ok");
    expect_equal(loaded.contents.manifest.market_count, 1U, "market count");
    expect_equal(loaded.contents.manifest.asset_count, 2U, "asset count");
    expect_equal(
        loaded.contents.manifest.input_snapshot_hash,
        std::string{"input_hash"},
        "input hash"
    );
    expect_equal(
        loaded.contents.market_universe_json,
        std::string{"{\"markets\":[\"m1\"]}\n"},
        "market universe"
    );
    expect_equal(loaded.contents.variables_bin, bytes({1, 2, 3}), "variables");
    expect_equal(
        loaded.contents.candidate_bundles_bin,
        bytes({9}),
        "candidate bundles"
    );
}

void ArtifactLoader_RejectsChecksumMismatch() {
    const auto artifact_dir = export_fixture("artifact_loader_mismatch");
    {
        std::ofstream output(
            artifact_dir / "rulebook.json",
            std::ios::binary | std::ios::app
        );
        output << "corrupt";
    }

    ArtifactLoader loader;
    const auto loaded = loader.load(artifact_dir);

    expect_false(loaded.ok(), "load ok");
    expect_false(loaded.checksums_ok, "checksums ok");
    expect_false(loaded.errors.empty(), "errors");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"ArtifactLoader_RoundTripsArtifact", &ArtifactLoader_RoundTripsArtifact},
        {
            "ArtifactLoader_RejectsChecksumMismatch",
            &ArtifactLoader_RejectsChecksumMismatch
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
