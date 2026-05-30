#include "oracle/bundles/CandidateBundleGenerator.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

using trading_engine::oracle::CandidateBundleGenerator;
using trading_engine::oracle::RawMarketRecord;

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

std::filesystem::path fixture_path() {
    return std::filesystem::path{POLYTOPE_SOURCE_DIR} /
           "tests/fixtures/oracle/candidate_bundles.json";
}

std::unordered_set<std::string> known_markets() {
    return {"m1"};
}

std::unordered_set<std::string> known_assets() {
    return {"asset_yes", "asset_no"};
}

void CandidateBundleGenerator_LoadsFixtureBundles() {
    CandidateBundleGenerator generator;
    const auto result = generator.load_fixture(
        fixture_path().string(),
        known_markets(),
        known_assets()
    );

    expect_true(result.ok(), "load ok");
    expect_equal(result.bundles.size(), 1U, "bundle count");
    expect_equal(result.bundles.front().bundle_id, 1ULL, "bundle id");
    expect_equal(result.bundles.front().leg_count, 2U, "leg count");
    expect_true(result.bundle_hash != 0, "bundle hash");
}

void CandidateBundleGenerator_DeterministicHash() {
    CandidateBundleGenerator generator;
    const auto a = generator.load_fixture(
        fixture_path().string(),
        known_markets(),
        known_assets()
    );
    const auto b = generator.load_fixture(
        fixture_path().string(),
        known_markets(),
        known_assets()
    );

    expect_true(a.ok(), "load a ok");
    expect_true(b.ok(), "load b ok");
    expect_equal(a.bundle_hash, b.bundle_hash, "bundle hash");
}

void CandidateBundleGenerator_ExportsArtifact() {
    CandidateBundleGenerator generator;
    const auto loaded = generator.load_fixture(
        fixture_path().string(),
        known_markets(),
        known_assets()
    );
    expect_true(loaded.ok(), "load ok");

    const auto out_path =
        std::filesystem::temp_directory_path() /
        "oracle_candidate_bundles_artifact.json";
    std::vector<std::string> errors;
    expect_true(
        generator.export_fixture_artifact(
            loaded.bundles,
            out_path.string(),
            &errors
        ),
        "export artifact"
    );
    expect_true(errors.empty(), "export errors");
    expect_true(std::filesystem::exists(out_path), "artifact exists");
    std::filesystem::remove(out_path);
}

RawMarketRecord raw_market(
    std::string market_id,
    std::string description
) {
    RawMarketRecord record;
    record.market_id = std::move(market_id);
    record.title = "Fixture market";
    record.description = std::move(description);
    record.outcomes = {"Yes", "No"};
    record.asset_ids = {"asset_yes", "asset_no"};
    return record;
}

void CandidateBundleGenerator_GeneratesBuyAllOutcomesBundle() {
    CandidateBundleGenerator generator;
    const auto result = generator.generate_buy_all_outcomes(
        {raw_market("m1", "This market resolves Yes or No.")},
        known_markets(),
        known_assets()
    );

    expect_true(result.ok(), "generate ok");
    expect_equal(result.bundles.size(), 1U, "bundle count");
    expect_equal(result.bundles.front().leg_count, 2U, "leg count");
    expect_equal(result.bundles.front().legs[0].asset_id, std::string{"asset_yes"}, "asset yes");
    expect_equal(result.bundles.front().legs[1].asset_id, std::string{"asset_no"}, "asset no");
    expect_true(result.bundle_hash != 0, "bundle hash");
}

void CandidateBundleGenerator_SkipsSplitResolutionText() {
    CandidateBundleGenerator generator;
    const auto result = generator.generate_buy_all_outcomes(
        {raw_market("m1", "If neither occurs, this market resolves to 50-50.")},
        known_markets(),
        known_assets()
    );

    expect_true(result.ok(), "generate ok");
    expect_true(result.bundles.empty(), "bundle skipped");
    expect_true(!result.warnings.empty(), "warning");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "CandidateBundleGenerator_LoadsFixtureBundles",
            &CandidateBundleGenerator_LoadsFixtureBundles
        },
        {
            "CandidateBundleGenerator_DeterministicHash",
            &CandidateBundleGenerator_DeterministicHash
        },
        {
            "CandidateBundleGenerator_ExportsArtifact",
            &CandidateBundleGenerator_ExportsArtifact
        },
        {
            "CandidateBundleGenerator_GeneratesBuyAllOutcomesBundle",
            &CandidateBundleGenerator_GeneratesBuyAllOutcomesBundle
        },
        {
            "CandidateBundleGenerator_SkipsSplitResolutionText",
            &CandidateBundleGenerator_SkipsSplitResolutionText
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
