#include "oracle/artifact/ArtifactExporter.h"
#include "oracle/artifact/ArtifactLoader.h"

#include <exception>
#include <filesystem>
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
    contents.manifest.created_at_ns = 19;
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
    contents.variables_bin = bytes({1, 2});
    contents.constraints_bin = bytes({3, 4});
    contents.feasible_states_bin = bytes({5, 6});
    contents.payoff_matrix_bin = bytes({7, 8});
    contents.candidate_bundles_bin = bytes({9, 10});
    contents.market_dependency_graph_bin = bytes({11});
    contents.settlement_bitmask_bin = bytes({12});
    return contents;
}

void OracleArtifactRoundTrip_WritesAndLoads() {
    const auto root =
        std::filesystem::temp_directory_path() /
        "oracle_artifact_roundtrip_integration";
    std::filesystem::remove_all(root);

    ArtifactExporter exporter;
    const auto exported = exporter.export_artifact(
        root,
        "artifact_a",
        fixture_contents()
    );
    expect_true(exported.ok(), "export ok");
    expect_true(exported.artifact_hash != 0, "artifact hash");

    ArtifactLoader loader;
    const auto loaded = loader.load(exported.artifact_dir);
    expect_true(loaded.ok(), "load ok");
    expect_true(loaded.checksums_ok, "checksums ok");
    expect_equal(loaded.checksums, exported.checksums, "checksums");
    expect_equal(
        loaded.contents.manifest.bundle_hash,
        std::string{"bundle_hash"},
        "bundle hash"
    );
    expect_equal(
        loaded.contents.constraints_bin,
        bytes({3, 4}),
        "constraints"
    );
    expect_equal(
        loaded.contents.settlement_bitmask_bin,
        bytes({12}),
        "settlement bitmask"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "OracleArtifactRoundTrip_WritesAndLoads",
            &OracleArtifactRoundTrip_WritesAndLoads
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
