#include "engine/signal/reader/OracleArtifactReader.h"

#include "oracle/artifact/ArtifactExporter.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::oracle::ArtifactExporter;
using trading_engine::oracle::BundleLeg;
using trading_engine::oracle::CandidateBundle;
using trading_engine::oracle::OracleArtifactContents;
using trading_engine::oracle::Side;
using trading_engine::signal::OracleArtifactReader;

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

void append_u8(std::vector<std::byte>* out, std::uint8_t value) {
    out->push_back(static_cast<std::byte>(value));
}

void append_u32(std::vector<std::byte>* out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        append_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::byte>* out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        append_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_i64(std::vector<std::byte>* out, std::int64_t value) {
    append_u64(out, static_cast<std::uint64_t>(value));
}

void append_string(std::vector<std::byte>* out, const std::string& value) {
    append_u32(out, static_cast<std::uint32_t>(value.size()));
    for (const unsigned char byte : value) {
        append_u8(out, byte);
    }
}

CandidateBundle bundle(std::uint64_t bundle_id) {
    CandidateBundle out;
    out.bundle_id = bundle_id;
    out.required_true_mask = 1;
    out.guaranteed_payout_tick = 1'000'000;
    out.min_edge_tick = 0;
    out.leg_count = 2;
    out.legs[0] = BundleLeg{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .side = Side::Buy,
        .quantity_lots = 1,
        .max_price_tick = 500000
    };
    out.legs[1] = BundleLeg{
        .market_id = "m1",
        .asset_id = "asset_no",
        .side = Side::Buy,
        .quantity_lots = 1,
        .max_price_tick = 500000
    };
    return out;
}

std::vector<std::byte> serialize_bundles(
    const std::vector<CandidateBundle>& bundles
) {
    std::vector<std::byte> out;
    append_u32(&out, static_cast<std::uint32_t>(bundles.size()));
    for (const auto& candidate : bundles) {
        append_u64(&out, candidate.bundle_id);
        append_u64(&out, candidate.required_true_mask);
        append_u64(&out, candidate.required_false_mask);
        append_u64(&out, candidate.invalid_mask);
        append_i64(&out, candidate.guaranteed_payout_tick);
        append_u32(&out, candidate.leg_count);
        append_i64(&out, candidate.min_edge_tick);
        for (std::uint16_t i = 0; i < candidate.leg_count; ++i) {
            const auto& leg = candidate.legs[i];
            append_string(&out, leg.market_id);
            append_string(&out, leg.asset_id);
            append_u8(&out, static_cast<std::uint8_t>(leg.side));
            append_i64(&out, leg.quantity_lots);
            append_i64(&out, leg.max_price_tick);
        }
    }
    return out;
}

std::filesystem::path source_path(const std::string& relative) {
    return std::filesystem::path{POLYTOPE_SOURCE_DIR} / relative;
}

std::filesystem::path test_root(const std::string& name) {
    const auto root =
        std::filesystem::temp_directory_path() / ("signal_" + name);
    std::filesystem::remove_all(root);
    return root;
}

std::filesystem::path export_artifact(
    const std::string& name,
    std::vector<CandidateBundle> bundles,
    std::uint32_t artifact_version = 1
) {
    OracleArtifactContents contents;
    contents.manifest.artifact_version = artifact_version;
    contents.manifest.created_at_ns = 1;
    contents.manifest.market_count = 1;
    contents.manifest.asset_count = 2;
    contents.manifest.variable_count = 2;
    contents.manifest.rule_count = 1;
    contents.manifest.constraint_count = 1;
    contents.manifest.feasible_state_count = 2;
    contents.manifest.bundle_count =
        static_cast<std::uint64_t>(bundles.size());
    contents.manifest.llm_provider = "none";
    contents.market_universe_json = "{\"markets\":[\"m1\"]}\n";
    contents.rulebook_json = "{\"rules\":[\"r1\"]}\n";
    contents.variables_bin = {std::byte{1}};
    contents.constraints_bin = {std::byte{2}};
    contents.feasible_states_bin = {std::byte{3}};
    contents.payoff_matrix_bin = {std::byte{4}};
    contents.candidate_bundles_bin = serialize_bundles(bundles);
    contents.market_dependency_graph_bin = {};
    contents.settlement_bitmask_bin = {};

    ArtifactExporter exporter;
    const auto result = exporter.export_artifact(
        test_root(name),
        "artifact",
        contents
    );
    expect_true(result.ok(), "export artifact");
    return result.artifact_dir;
}

void OracleArtifactReader_LoadsFixtureBundles() {
    OracleArtifactReader reader;
    const auto result = reader.load(
        source_path("tests/fixtures/oracle/artifact_small")
    );

    expect_true(result.ok, "load ok: " + result.error);
    expect_equal(result.artifact_version, 1ULL, "artifact_version");
    expect_equal(result.bundle_count, 1ULL, "bundle_count");
    expect_equal(reader.artifact_version(), 1ULL, "reader version");
    expect_true(reader.bundle_hash() != 0, "bundle_hash");
    expect_equal(reader.active_bundles().size(), 1U, "active bundle count");
    expect_equal(
        reader.active_bundles()[0].legs[0].asset_id,
        std::string{"asset_yes"},
        "asset id"
    );
}

void OracleArtifactReader_RejectsMissingManifest() {
    const auto artifact_dir = export_artifact(
        "oracle_reader_missing_manifest",
        {bundle(1)}
    );
    std::filesystem::remove(artifact_dir / "manifest.json");

    OracleArtifactReader reader;
    const auto result = reader.load(artifact_dir);

    expect_false(result.ok, "load ok");
    expect_false(result.error.empty(), "error");
}

void OracleArtifactReader_RejectsChecksumMismatch() {
    const auto artifact_dir = export_artifact(
        "oracle_reader_checksum_mismatch",
        {bundle(1)}
    );
    {
        std::ofstream output(
            artifact_dir / "candidate_bundles.bin",
            std::ios::binary | std::ios::app
        );
        output << "corrupt";
    }

    OracleArtifactReader reader;
    const auto result = reader.load(artifact_dir);

    expect_false(result.ok, "load ok");
    expect_false(result.error.empty(), "error");
}

void OracleArtifactReader_RejectsUnsupportedVersion() {
    const auto artifact_dir = export_artifact(
        "oracle_reader_unsupported_version",
        {bundle(1)},
        999
    );

    OracleArtifactReader reader;
    const auto result = reader.load(artifact_dir);

    expect_false(result.ok, "load ok");
    expect_equal(
        result.artifact_version,
        999ULL,
        "reported artifact_version"
    );
}

void OracleArtifactReader_RejectsDuplicateBundleId() {
    const auto artifact_dir = export_artifact(
        "oracle_reader_duplicate_bundle",
        {bundle(1), bundle(1)}
    );

    OracleArtifactReader reader;
    const auto result = reader.load(artifact_dir);

    expect_false(result.ok, "load ok");
    expect_false(result.error.empty(), "error");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "OracleArtifactReader_LoadsFixtureBundles",
            &OracleArtifactReader_LoadsFixtureBundles
        },
        {
            "OracleArtifactReader_RejectsMissingManifest",
            &OracleArtifactReader_RejectsMissingManifest
        },
        {
            "OracleArtifactReader_RejectsChecksumMismatch",
            &OracleArtifactReader_RejectsChecksumMismatch
        },
        {
            "OracleArtifactReader_RejectsUnsupportedVersion",
            &OracleArtifactReader_RejectsUnsupportedVersion
        },
        {
            "OracleArtifactReader_RejectsDuplicateBundleId",
            &OracleArtifactReader_RejectsDuplicateBundleId
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
