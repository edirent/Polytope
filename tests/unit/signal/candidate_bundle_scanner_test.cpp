#include "engine/signal/reader/OracleArtifactReader.h"
#include "engine/signal/scan/BundleRegistry.h"
#include "engine/signal/scan/CandidateBundleScanner.h"
#include "engine/signal/scan/DirtyAssetSet.h"
#include "tests/unit/signal/signal_test_artifact_utils.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::signal::BundleRegistry;
using trading_engine::signal::CandidateBundleScanner;
using trading_engine::signal::DirtyAssetSet;
using trading_engine::signal::OracleArtifactReader;

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

BundleRegistry registry() {
    const auto artifact_dir = signal_test::export_artifact(
        "candidate_bundle_scanner",
        {
            signal_test::make_bundle(3, {"asset_c", "asset_d"}),
            signal_test::make_bundle(1, {"asset_a", "asset_b"}),
            signal_test::make_bundle(2, {"asset_b", "asset_c"})
        }
    );
    expect_true(!artifact_dir.empty(), "artifact exported");

    OracleArtifactReader reader;
    const auto loaded = reader.load(artifact_dir);
    expect_true(loaded.ok, "reader load: " + loaded.error);

    BundleRegistry out;
    expect_true(out.load_from_oracle_reader(reader), "registry load");
    return out;
}

void CandidateBundleScanner_FullScanReturnsAll() {
    const auto registry_value = registry();
    CandidateBundleScanner scanner;
    const auto bundles = scanner.full_scan(registry_value);

    expect_equal(bundles.size(), 3U, "bundle count");
    expect_equal(bundles[0]->bundle_id, 1ULL, "bundle 0");
    expect_equal(bundles[1]->bundle_id, 2ULL, "bundle 1");
    expect_equal(bundles[2]->bundle_id, 3ULL, "bundle 2");
}

void CandidateBundleScanner_DirtyAssetReturnsAffectedBundlesOnly() {
    const auto registry_value = registry();
    DirtyAssetSet dirty_assets;
    dirty_assets.mark_dirty("asset_b");
    dirty_assets.mark_dirty("asset_b");
    dirty_assets.mark_dirty("asset_missing");

    CandidateBundleScanner scanner;
    const auto bundles = scanner.dirty_asset_scan(registry_value, dirty_assets);

    expect_equal(bundles.size(), 2U, "bundle count");
    expect_equal(bundles[0]->bundle_id, 1ULL, "bundle 0");
    expect_equal(bundles[1]->bundle_id, 2ULL, "bundle 1");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "CandidateBundleScanner_FullScanReturnsAll",
            &CandidateBundleScanner_FullScanReturnsAll
        },
        {
            "CandidateBundleScanner_DirtyAssetReturnsAffectedBundlesOnly",
            &CandidateBundleScanner_DirtyAssetReturnsAffectedBundlesOnly
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
