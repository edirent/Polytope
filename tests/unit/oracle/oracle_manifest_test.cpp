#include "oracle/public/ArtifactManifest.h"
#include "oracle/public/OracleError.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::ArtifactManifest;
using trading_engine::oracle::OracleErrorCode;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
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

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
    }
}

void ArtifactManifest_DefaultsAreSafe() {
    const ArtifactManifest manifest;

    expect_equal(manifest.artifact_version, 1U, "artifact_version");
    expect_equal(manifest.created_at_ns, 0ULL, "created_at_ns");
    expect_equal(manifest.market_count, 0U, "market_count");
    expect_equal(manifest.asset_count, 0U, "asset_count");
    expect_equal(manifest.variable_count, 0U, "variable_count");
    expect_equal(manifest.rule_count, 0U, "rule_count");
    expect_equal(manifest.constraint_count, 0U, "constraint_count");
    expect_equal(
        manifest.feasible_state_count,
        0ULL,
        "feasible_state_count"
    );
    expect_equal(manifest.bundle_count, 0ULL, "bundle_count");
    expect_false(manifest.llm_enabled, "llm_enabled");
    expect_false(manifest.llm_outputs_used, "llm_outputs_used");
    expect_false(
        manifest.llm_outputs_require_manual_review,
        "llm_outputs_require_manual_review"
    );
    expect_equal(manifest.llm_provider, std::string{"none"}, "llm_provider");
    expect_equal(
        manifest.input_snapshot_hash,
        std::string{},
        "input_snapshot_hash"
    );
    expect_equal(manifest.rulebook_hash, std::string{}, "rulebook_hash");
    expect_equal(manifest.constraint_hash, std::string{}, "constraint_hash");
    expect_equal(
        manifest.feasible_states_hash,
        std::string{},
        "feasible_states_hash"
    );
    expect_equal(manifest.payoff_hash, std::string{}, "payoff_hash");
    expect_equal(manifest.bundle_hash, std::string{}, "bundle_hash");
}

void OracleErrorCode_DefaultNone() {
    OracleErrorCode code{};
    expect_equal(code, OracleErrorCode::None, "default error code");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "ArtifactManifest_DefaultsAreSafe",
            &ArtifactManifest_DefaultsAreSafe
        },
        {"OracleErrorCode_DefaultNone", &OracleErrorCode_DefaultNone}
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
