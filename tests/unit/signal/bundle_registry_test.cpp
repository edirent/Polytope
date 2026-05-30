#include "engine/signal/reader/OracleArtifactReader.h"
#include "engine/signal/scan/BundleRegistry.h"
#include "tests/unit/signal/signal_test_artifact_utils.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::signal::BundleRegistry;
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

BundleRegistry registry_from_bundles(
    const std::vector<signal_test::CandidateBundle>& bundles
) {
    const auto artifact_dir =
        signal_test::export_artifact("bundle_registry", bundles);
    expect_true(!artifact_dir.empty(), "artifact exported");

    OracleArtifactReader reader;
    const auto loaded = reader.load(artifact_dir);
    expect_true(loaded.ok, "reader load: " + loaded.error);

    BundleRegistry registry;
    expect_true(registry.load_from_oracle_reader(reader), "registry load");
    return registry;
}

void BundleRegistry_LoadsBundles() {
    const auto registry = registry_from_bundles({
        signal_test::make_bundle(2, {"asset_b", "asset_c"}),
        signal_test::make_bundle(1, {"asset_a", "asset_b"})
    });

    expect_equal(registry.active_bundles().size(), 2U, "bundle count");
    expect_equal(registry.active_bundles()[0].bundle_id, 1ULL, "bundle 0");
    expect_equal(registry.active_bundles()[1].bundle_id, 2ULL, "bundle 1");
    expect_true(registry.artifact_hash() != 0, "artifact hash");
}

void BundleRegistry_BuildsAssetIndex() {
    const auto registry = registry_from_bundles({
        signal_test::make_bundle(1, {"asset_a", "asset_b"}),
        signal_test::make_bundle(2, {"asset_b", "asset_c"})
    });

    const auto a = registry.bundles_for_asset("asset_a");
    const auto b = registry.bundles_for_asset("asset_b");
    const auto missing = registry.bundles_for_asset("asset_missing");

    expect_equal(a.size(), 1U, "asset_a count");
    expect_equal(a[0], 1ULL, "asset_a bundle");
    expect_equal(b.size(), 2U, "asset_b count");
    expect_equal(b[0], 1ULL, "asset_b bundle 0");
    expect_equal(b[1], 2ULL, "asset_b bundle 1");
    expect_true(missing.empty(), "missing asset");
}

void BundleRegistry_FindsBundleById() {
    const auto registry = registry_from_bundles({
        signal_test::make_bundle(1, {"asset_a"}),
        signal_test::make_bundle(2, {"asset_b"})
    });

    const auto* bundle = registry.find_bundle(2);
    expect_true(bundle != nullptr, "find bundle");
    expect_equal(bundle->bundle_id, 2ULL, "bundle id");
    expect_true(registry.find_bundle(999) == nullptr, "missing bundle");
    expect_true(registry.bundle_hash(2) != 0, "bundle hash");
    expect_equal(registry.bundle_hash(999), 0ULL, "missing bundle hash");
}

void BundleRegistry_RejectsDuplicateBundleId() {
    const auto artifact_dir = signal_test::export_artifact(
        "bundle_registry_duplicate",
        {
            signal_test::make_bundle(1, {"asset_a"}),
            signal_test::make_bundle(1, {"asset_b"})
        }
    );
    expect_true(!artifact_dir.empty(), "artifact exported");

    OracleArtifactReader reader;
    const auto loaded = reader.load(artifact_dir);
    expect_false(loaded.ok, "reader load duplicate");

    BundleRegistry registry;
    expect_false(registry.load_from_oracle_reader(reader), "registry load");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"BundleRegistry_LoadsBundles", &BundleRegistry_LoadsBundles},
        {
            "BundleRegistry_BuildsAssetIndex",
            &BundleRegistry_BuildsAssetIndex
        },
        {"BundleRegistry_FindsBundleById", &BundleRegistry_FindsBundleById},
        {
            "BundleRegistry_RejectsDuplicateBundleId",
            &BundleRegistry_RejectsDuplicateBundleId
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
