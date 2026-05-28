#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail("failed to open output: " + path.string());
    }
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
}

std::filesystem::path source_path(const std::string& relative) {
    return std::filesystem::path{POLYTOPE_SOURCE_DIR} / relative;
}

std::string base_command(
    const std::filesystem::path& out,
    const std::filesystem::path& output
) {
    return std::string{VERIFY_ORACLE_WORKFLOW_BIN} +
           " --market-snapshot " +
           source_path("tests/fixtures/oracle/raw_markets_small.jsonl").string() +
           " --rulebook " +
           source_path("tests/fixtures/oracle/rulebook_small.json").string() +
           " --candidate-bundles " +
           source_path("tests/fixtures/oracle/candidate_bundles_small.json").string() +
           " --out " + out.string() +
           " --check-determinism > " + output.string() + " 2>&1";
}

void OracleWorkflow_SmallFixtureCompiles() {
    const auto root =
        std::filesystem::temp_directory_path() /
        "oracle_workflow_small_fixture";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto output = root / "workflow.out";

    const int rc = std::system(base_command(root / "artifact", output).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_true(text.find("markets_loaded: 1") != std::string::npos, "markets");
    expect_true(text.find("assets_loaded: 2") != std::string::npos, "assets");
    expect_true(text.find("approved_rules: 1") != std::string::npos, "rules");
    expect_true(text.find("variables: 2") != std::string::npos, "variables");
    expect_true(text.find("constraints: 1") != std::string::npos, "constraints");
    expect_true(text.find("feasible_states: 2") != std::string::npos, "states");
    expect_true(text.find("manifest_ok: true") != std::string::npos, "manifest");
    expect_true(text.find("checksums_ok: true") != std::string::npos, "checksums");
}

void OracleWorkflow_LlmDisabledStillPasses() {
    const auto root =
        std::filesystem::temp_directory_path() /
        "oracle_workflow_llm_disabled";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto output = root / "workflow.out";

    const int rc = std::system(base_command(root / "artifact", output).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_true(text.find("enabled: false") != std::string::npos, "llm enabled");
    expect_true(text.find("provider: none") != std::string::npos, "llm provider");
    expect_true(
        text.find("outputs_used: false") != std::string::npos,
        "llm outputs"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "OracleWorkflow_SmallFixtureCompiles",
            &OracleWorkflow_SmallFixtureCompiles
        },
        {
            "OracleWorkflow_LlmDisabledStillPasses",
            &OracleWorkflow_LlmDisabledStillPasses
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
