#include "tests/integration/decision_fastpath/fastpath_test_helpers.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

#ifndef POLYTOPE_SOURCE_DIR
#define POLYTOPE_SOURCE_DIR "."
#endif

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void FastPathShadowWorldcup30m_MismatchZero() {
    const std::filesystem::path artifact{
        std::filesystem::path{POLYTOPE_SOURCE_DIR} /
        "runs/worldcup_30615_full_20260530_062519/"
        "oracle_artifact_30615_top8"
    };
    const std::filesystem::path raw{
        std::filesystem::path{POLYTOPE_SOURCE_DIR} /
        "runs/worldcup_feed_to_execute_30m_20260530_170629/feed.raw"
    };
    if (!std::filesystem::exists(artifact) || !std::filesystem::exists(raw)) {
        fail("worldcup 30m replay/artifact fixture missing");
    }

    const auto registry =
        decision_fastpath_test::registry_from_artifact(artifact);
    if (registry.specs().empty()) {
        fail("worldcup artifact has no fastpath specs");
    }
    const auto policy = decision_fastpath_test::policy();
    const auto depths = decision_fastpath_test::depths_for_specs(
        registry.specs()
    );

    decision_fastpath_test::expect_shadow_matches_generic_for_all_dirty_assets(
        registry,
        policy,
        depths
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"FastPathShadowWorldcup30m_MismatchZero",
         &FastPathShadowWorldcup30m_MismatchZero},
    };
    return test_map;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }
    const auto it = tests().find(argv[1]);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << argv[1] << '\n';
        return 2;
    }
    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << argv[1] << " failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
