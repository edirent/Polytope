#include "oracle/public/ArtifactManifest.h"
#include "oracle/public/CandidateBundle.h"
#include "oracle/public/OracleTypes.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::ArtifactManifest;
using trading_engine::oracle::CandidateBundle;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
    }
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void OracleBuild_NoLlmRequired() {
#if ORACLE_ENABLE_LLM != 0
    fail("ORACLE_ENABLE_LLM must default to OFF");
#endif

    expect_false(
        trading_engine::oracle::kLlmBuildEnabled,
        "kLlmBuildEnabled"
    );

    (void)std::getenv("ANTHROPIC_API_KEY");

    ArtifactManifest manifest;
    CandidateBundle bundle;
    expect_false(manifest.llm_enabled, "manifest llm_enabled");
    expect_true(bundle.leg_count == 0, "bundle starts empty");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"OracleBuild_NoLlmRequired", &OracleBuild_NoLlmRequired}
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
